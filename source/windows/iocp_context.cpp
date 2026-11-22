/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "iocp_context.hpp"
#include <algorithm>
#include <chrono>
#include "common/handshake.hpp"
#include "common/spin.hpp"
#include "path.hpp"
#include "pipe_connection.hpp"
#include "pipe_server.hpp"
#include "shm.hpp"

#ifndef FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#endif
#ifndef FILE_SKIP_SET_EVENT_ON_HANDLE
#define FILE_SKIP_SET_EVENT_ON_HANDLE 0x2
#endif

namespace {
	// The context whose I/O thread is running on this thread, if any.
	thread_local const datapath::windows::iocp_context* tl_context = nullptr;
	thread_local int                                     tl_index   = 0;

	using rtl_nt_status_to_dos_error_t = ULONG(WINAPI*)(LONG);

	DWORD status_to_error(ULONG_PTR status)
	{
		if (status == 0)
			return ERROR_SUCCESS;
		static auto fn = reinterpret_cast<rtl_nt_status_to_dos_error_t>(
			reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError")));
		return fn ? DWORD(fn(LONG(status))) : ERROR_GEN_FAILURE;
	}

	// Win11 may run background apps' threads in EcoQoS (efficiency cores, low
	// clocks). Opt the I/O threads out. Absent before Windows 10 1709: ignored.
	void disable_power_throttling()
	{
		struct throttling_state {
			ULONG Version;
			ULONG ControlMask;
			ULONG StateMask;
		};
		using set_thread_information_t = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
		static auto fn                 = reinterpret_cast<set_thread_information_t>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation")));
		if (!fn)
			return;
		throttling_state state{1 /*THREAD_POWER_THROTTLING_CURRENT_VERSION*/,
							   1 /*THREAD_POWER_THROTTLING_EXECUTION_SPEED*/, 0};
		fn(GetCurrentThread(), 3 /*ThreadPowerThrottling*/, &state, sizeof(state));
	}
	// First logical processor of every physical core, in system order.
	// Called on the thread that creates the context, never on an I/O thread: a
	// function-local static here would register its destructor from the I/O
	// thread, which deadlocks against a process exiting (and joining it).
	std::vector<PROCESSOR_NUMBER> physical_cores()
	{
		{
			std::vector<PROCESSOR_NUMBER> out;
			DWORD                         len = 0;
			GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
			std::vector<uint8_t> buf(len);
			auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
			if (!len || !GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len))
				return out;
			for (DWORD off = 0; off < len;) {
				auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
				if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount > 0) {
					const GROUP_AFFINITY& g    = e->Processor.GroupMask[0];
					KAFFINITY             mask = g.Mask;
					BYTE                  bit  = 0;
					while (mask && !(mask & 1)) {
						mask >>= 1;
						bit++;
					}
					if (mask)
						out.push_back(PROCESSOR_NUMBER{g.Group, bit, 0});
				}
				off += e->Size;
			}
			return out;
		}
	}

	// Logical processors of physical core 0 in processor group 0 (0 if unknown).
	// Called on the creating thread, like physical_cores().
	KAFFINITY core0_mask()
	{
		DWORD len = 0;
		GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
		std::vector<uint8_t> buf(len);
		auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
		if (!len || !GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len))
			return 0;
		for (DWORD off = 0; off < len;) {
			auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
			if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount > 0
				&& e->Processor.GroupMask[0].Group == 0 && (e->Processor.GroupMask[0].Mask & 1))
				return e->Processor.GroupMask[0].Mask;
			off += e->Size;
		}
		return 0;
	}

	// Logical processors (group 0) of the physical core that holds `pn`; 0 if
	// unknown or not in group 0. Called on the creating thread.
	KAFFINITY core_mask_of(const PROCESSOR_NUMBER& pn)
	{
		if (pn.Group != 0)
			return 0;
		DWORD len = 0;
		GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
		std::vector<uint8_t> buf(len);
		auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
		if (!len || !GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len))
			return 0;
		for (DWORD off = 0; off < len;) {
			auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
			if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount > 0
				&& e->Processor.GroupMask[0].Group == 0
				&& (e->Processor.GroupMask[0].Mask & (KAFFINITY(1) << pn.Number)))
				return e->Processor.GroupMask[0].Mask;
			off += e->Size;
		}
		return 0;
	}

	int lowest_bit(DWORD_PTR m)
	{
		for (int i = 0; i < int(sizeof(m) * 8); i++)
			if (m & (DWORD_PTR(1) << i))
				return i;
		return -1;
	}

	// Physical cores that I/O threads are spread over: all of them, minus core 0
	// (interrupt/DPC heavy) when there are more than two.
	size_t first_io_core(size_t cores)
	{
		return cores > 2 ? 1 : 0;
	}

	// The physical core (its first logical processor) that I/O thread `index`
	// prefers. Automatic (ideal_core -1, also used for pairing when placement is
	// off) skips core 0 (interrupt/DPC heavy) when there is room.
	PROCESSOR_NUMBER pick_core(int ideal_core, const std::vector<PROCESSOR_NUMBER>& cores, size_t index)
	{
		if (ideal_core >= 0)
			return cores[(size_t(ideal_core) + index) % cores.size()];
		size_t first = first_io_core(cores.size());
		size_t span  = cores.size() - first;
		size_t start = size_t(GetCurrentProcessId() * 2654435761u) % span;
		return cores[first + (start + index) % span];
	}

	void apply_ideal_core(const datapath::context_options& opts, const std::vector<PROCESSOR_NUMBER>& cores,
						  KAFFINITY avoid, size_t index)
	{
		if (opts.ideal_core < -1)
			return;
		if (cores.empty())
			return;
		PROCESSOR_NUMBER pn = pick_core(opts.ideal_core, cores, index);
		if (opts.ideal_core < 0) {
			size_t first = first_io_core(cores.size());
			// The ideal processor is only a hint, and under full load Windows still
			// ran I/O threads on core 0, where interrupts and DPCs land: 16 KiB
			// p99.9 1.1 ms there vs ~0.2 ms on other cores (Irkela, --stress 0).
			// Keep them off it for real, within the process's own affinity. Single
			// processor group only; a process confined to core 0 stays as it is.
			DWORD_PTR process = 0, system = 0;
			if (first > 0 && avoid && pn.Group == 0 && GetActiveProcessorGroupCount() == 1
				&& GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
				DWORD_PTR allowed = process & ~DWORD_PTR(avoid);
				if (allowed)
					SetThreadAffinityMask(GetCurrentThread(), allowed);
			}
		}
		SetThreadIdealProcessorEx(GetCurrentThread(), &pn, nullptr);
	}

	// Scheduling tweaks for an I/O thread. Returns the MMCSS handle to revert.
	HANDLE apply_thread_scheduling(const datapath::context_options& opts)
	{
		switch (opts.priority) {
		case 1:
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
			break;
		case 2:
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
			break;
		case 3:
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			break;
		default:
			break;
		}
		if (!opts.mmcss)
			return nullptr;
		static HMODULE avrt = LoadLibraryW(L"avrt.dll");
		if (!avrt)
			return nullptr;
		using set_t      = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
		using priority_t = BOOL(WINAPI*)(HANDLE, int);
		auto set         = reinterpret_cast<set_t>(
            reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")));
		auto prio = reinterpret_cast<priority_t>(
			reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadPriority")));
		if (!set)
			return nullptr;
		DWORD  task = 0;
		HANDLE h    = set(L"Pro Audio", &task);
		if (h && prio)
			prio(h, 2 /*AVRT_PRIORITY_CRITICAL*/);
		return h;
	}

	void revert_thread_scheduling(HANDLE mmcss)
	{
		if (!mmcss)
			return;
		static HMODULE avrt   = GetModuleHandleW(L"avrt.dll");
		using revert_t        = BOOL(WINAPI*)(HANDLE);
		auto revert           = avrt ? reinterpret_cast<revert_t>(reinterpret_cast<void*>(
                                     GetProcAddress(avrt, "AvRevertMmThreadCharacteristics")))
									 : nullptr;
		if (revert)
			revert(mmcss);
	}
} // namespace

datapath::windows::iocp_context::iocp_context(const datapath::context_options& opts) : _opts(opts)
{
	if (_opts.io_threads == 0) {
		// Automatic: one I/O thread per physical core that ideal_core would use.
		auto cores       = physical_cores();
		_opts.io_threads = cores.size() - first_io_core(cores.size());
	}
	if (_opts.io_threads == 0)
		_opts.io_threads = 1;
}

datapath::windows::iocp_context::~iocp_context()
{
	shutdown();
	if (_port)
		CloseHandle(_port);
}

bool datapath::windows::iocp_context::start()
{
	_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, DWORD(_opts.io_threads));
	if (!_port)
		return false;
	// Each thread keeps its context alive, so the context is never destroyed on
	// one of its own I/O threads (dispatch and run keep using `this`).
	std::vector<PROCESSOR_NUMBER> cores;
	KAFFINITY                     avoid = 0;
	if (_opts.ideal_core >= -1 || _opts.pair_peer)
		cores = physical_cores();
	if (_opts.ideal_core == -1)
		avoid = core0_mask();
	if (_opts.pair_peer && _opts.io_threads == 1 && !cores.empty() && GetActiveProcessorGroupCount() == 1) {
		// Pair on the core the I/O thread prefers anyway; with placement off, on
		// the core automatic placement would pick.
		_pair_core = core_mask_of(pick_core(_opts.ideal_core >= 0 ? _opts.ideal_core : -1, cores, 0));
		DWORD_PTR process = 0, system = 0;
		if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
			_restore_mask = process;
			if (avoid && cores.size() > 2 && (process & ~DWORD_PTR(avoid)))
				_restore_mask = process & ~DWORD_PTR(avoid);
		}
	}
	for (size_t i = 0; i < _opts.io_threads; i++)
		_threads.emplace_back([this, i, cores, avoid, keep = shared_from_this()] {
			tl_context = keep.get();
			tl_index   = int(i);
			apply_ideal_core(_opts, cores, avoid, i);
			run();
		});
	return true;
}

bool datapath::windows::iocp_context::pin_io_thread(DWORD_PTR mask)
{
	if (!mask || _threads.size() != 1)
		return false;
	return SetThreadAffinityMask(_threads[0].native_handle(), mask) != 0;
}

int32_t datapath::windows::iocp_context::pair_server()
{
	if (!pair_wanted() || !_pair_core)
		return -1;
	DWORD_PTR process = 0, system = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
		return -1;
	// Two hyper-threads of the core: the lower one for us, the next for the client.
	DWORD_PTR both = DWORD_PTR(_pair_core) & process;
	int       own  = lowest_bit(both);
	int       peer = own < 0 ? -1 : lowest_bit(both & ~(DWORD_PTR(1) << own));
	if (peer < 0)
		return -1; // no SMT, or the process may only use one of them
	if (_paired.exchange(true, std::memory_order_acq_rel))
		return -1; // an earlier connection is paired already
	if (!pin_io_thread(DWORD_PTR(1) << own)) {
		_paired.store(false, std::memory_order_release);
		return -1;
	}
	return peer;
}

bool datapath::windows::iocp_context::pair_client(int32_t cpu)
{
	if (cpu < 0 || cpu >= int32_t(sizeof(DWORD_PTR) * 8) || !pair_wanted() || GetActiveProcessorGroupCount() != 1)
		return false;
	DWORD_PTR process = 0, system = 0;
	DWORD_PTR mask    = DWORD_PTR(1) << cpu;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !(process & mask))
		return false;
	if (_paired.exchange(true, std::memory_order_acq_rel))
		return false;
	if (!_restore_mask)
		_restore_mask = process;
	if (!pin_io_thread(mask)) {
		_paired.store(false, std::memory_order_release);
		return false;
	}
	return true;
}

void datapath::windows::iocp_context::unpair()
{
	if (!_paired.exchange(false, std::memory_order_acq_rel))
		return;
	DWORD_PTR mask = _restore_mask;
	if (!mask) {
		DWORD_PTR system = 0;
		GetProcessAffinityMask(GetCurrentProcess(), &mask, &system);
	}
	pin_io_thread(mask);
}

bool datapath::windows::iocp_context::associate(HANDLE h)
{
	if (!CreateIoCompletionPort(h, _port, 0, 0))
		return false;
	return SetFileCompletionNotificationModes(h, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE)
		   != FALSE;
}

void datapath::windows::iocp_context::op_finished() noexcept
{
	if (_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1 && stopped()) {
		std::lock_guard<std::mutex> l(_wait_lock);
		_wait_cv.notify_all();
	}
}

bool datapath::windows::iocp_context::post(io_op& op, std::shared_ptr<io_target> target)
{
	if (stopped())
		return false;
	op.reset();
	op.keep = std::move(target);
	op_started();
	if (!PostQueuedCompletionStatus(_port, 0, 0, &op)) {
		op.keep.reset();
		op_finished();
		return false;
	}
	return true;
}

void datapath::windows::iocp_context::dispatch(OVERLAPPED_ENTRY& entry)
{
	auto* op   = static_cast<io_op*>(entry.lpOverlapped);
	DWORD err  = status_to_error(op->Internal);
	auto  keep = std::move(op->keep);
	if (keep)
		keep->on_io(op, entry.dwNumberOfBytesTransferred, err);
	keep.reset();
	op_finished();
}

void datapath::windows::iocp_context::run()
{
	disable_power_throttling();
	HANDLE           mmcss = apply_thread_scheduling(_opts);
	OVERLAPPED_ENTRY entries[64];
	bool             quit = false;
	auto             pump = [&](DWORD timeout) {
        ULONG n = 0;
        // Alertable waits may return early with WAIT_IO_COMPLETION after an APC;
        // that is simply "no completions yet".
        if (!GetQueuedCompletionStatusEx(_port, entries, 64, &n, timeout, _opts.alertable ? TRUE : FALSE))
            return false;
        for (ULONG i = 0; i < n; i++) {
            if (!entries[i].lpOverlapped) {
                // One quit packet per thread: hand extras back to the others.
                if (quit)
                    PostQueuedCompletionStatus(_port, 0, 0, nullptr);
                quit = true;
                continue;
            }
            dispatch(entries[i]);
        }
        return n > 0;
	};
	// Shared-memory rings this thread busy-polls along with the port. The list
	// is copied from the context whenever it changes (never while idle).
	std::vector<std::shared_ptr<poll_target>> polled;
	uint64_t                                  polled_version = ~uint64_t(0);
	const int                                 me             = tl_index;
	auto                                      refresh        = [&] {
        uint64_t v = _poll_version.load(std::memory_order_acquire);
        if (v == polled_version)
            return;
        std::lock_guard<std::mutex> l(_poll_lock);
        polled         = _pollers;
        polled_version = _poll_version.load(std::memory_order_relaxed);
	};
	auto poll = [&] {
		bool work = pump(0);
		refresh();
		for (auto& t : polled)
			if (t->owner.load(std::memory_order_relaxed) == me)
				work |= t->poll();
		return work;
	};
	while (!quit) {
		auto block = [&] {
			if (quit)
				return;
			refresh();
			bool work = false;
			for (auto& t : polled)
				if (t->owner.load(std::memory_order_relaxed) == me && !t->prepare_block())
					work = true;
			if (work) {
				// Work arrived meanwhile: handle it here (with no spin budget this is
				// the only place rings get polled), then spin again.
				poll();
				return;
			}
			pump(INFINITE);
		};
		if (_opts.yield)
			datapath::detail::spin_then_block(_opts.spin, [&] { return !quit && poll(); }, block, [] { Sleep(0); });
		else
			datapath::detail::spin_then_block(_opts.spin, [&] { return !quit && poll(); }, block);
	}
	polled.clear();
	revert_thread_scheduling(mmcss);
}

void datapath::windows::iocp_context::add_member(std::weak_ptr<datapath::detail::connection> c)
{
	std::lock_guard<std::mutex> l(_members_lock);
	if (_connections.size() >= 64 && (_connections.size() & (_connections.size() - 1)) == 0)
		_connections.erase(std::remove_if(_connections.begin(), _connections.end(), [](auto& w) { return w.expired(); }),
						   _connections.end());
	_connections.push_back(std::move(c));
}

void datapath::windows::iocp_context::add_member(std::weak_ptr<datapath::detail::listener> l)
{
	std::lock_guard<std::mutex> g(_members_lock);
	_listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(), [](auto& w) { return w.expired(); }),
					 _listeners.end());
	_listeners.push_back(std::move(l));
}

int datapath::windows::iocp_context::thread_index() noexcept
{
	return tl_index;
}

void datapath::windows::iocp_context::add_poller(std::shared_ptr<poll_target> t)
{
	std::lock_guard<std::mutex> l(_poll_lock);
	_pollers.push_back(std::move(t));
	_poll_version.fetch_add(1, std::memory_order_release);
}

void datapath::windows::iocp_context::remove_poller(poll_target* t)
{
	std::lock_guard<std::mutex> l(_poll_lock);
	_pollers.erase(std::remove_if(_pollers.begin(), _pollers.end(), [t](auto& p) { return p.get() == t; }),
				   _pollers.end());
	_poll_version.fetch_add(1, std::memory_order_release);
}

bool datapath::windows::iocp_context::shared_memory_wanted(const datapath::options& opts) noexcept
{
	// DATAPATH_SHARED_MEMORY=1/0 overrides the option for the whole process.
	static const int forced = [] {
		char  v[4] = {};
		DWORD n    = GetEnvironmentVariableA("DATAPATH_SHARED_MEMORY", v, sizeof(v));
		return n > 0 && n < sizeof(v) ? (v[0] == '0' ? 0 : 1) : -1;
	}();
	bool want = forced < 0 ? opts.shared_memory : forced == 1;
	return want && shm_channel::supported();
}

bool datapath::windows::iocp_context::on_io_thread() const noexcept
{
	return tl_context == this;
}

void datapath::windows::iocp_context::shutdown()
{
	if (_stopped.exchange(true))
		return;
	if (on_io_thread()) {
		// An I/O thread cannot drain completions it is supposed to process, nor
		// join itself: finish on a separate thread.
		std::thread([self = shared_from_this()] { self->shutdown_now(); }).detach();
		return;
	}
	shutdown_now();
}

void datapath::windows::iocp_context::shutdown_now()
{

	std::vector<std::weak_ptr<datapath::detail::connection>> conns;
	std::vector<std::weak_ptr<datapath::detail::listener>>   lsts;
	{
		std::lock_guard<std::mutex> l(_members_lock);
		conns.swap(_connections);
		lsts.swap(_listeners);
	}
	for (auto& w : lsts)
		if (auto l = w.lock())
			l->close();
	for (auto& w : conns)
		if (auto c = w.lock())
			c->close();

	// Let aborted operations drain (bounded), then stop the threads.
	{
		std::unique_lock<std::mutex> l(_wait_lock);
		_wait_cv.wait_for(l, std::chrono::seconds(2), [this] { return _outstanding.load() <= 0; });
	}
	for (size_t i = 0; i < _threads.size(); i++)
		PostQueuedCompletionStatus(_port, 0, 0, nullptr);
	for (auto& t : _threads)
		if (t.joinable())
			t.join();
	_threads.clear();
	std::lock_guard<std::mutex> l(_poll_lock);
	_pollers.clear();
}

datapath::error datapath::windows::iocp_context::connect(std::shared_ptr<datapath::detail::connection>& out,
														 const std::string& path, const datapath::options& opts)
{
	if (stopped())
		return datapath::error::Closed;
	std::wstring wpath;
	if (!make_pipe_path(path, wpath))
		return datapath::error::InvalidPath;

	using clock   = std::chrono::steady_clock;
	auto deadline = clock::now() + opts.connect_timeout;
	auto remaining_ms = [&]() -> DWORD {
		auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
		return left > 0 ? DWORD(left) : 0;
	};

	HANDLE                  h        = INVALID_HANDLE_VALUE;
	datapath::detail::hello reply{};
	int32_t                 pair_cpu = -1;
	// Version 3 when asking for shared memory, else version 2 for pairing, else 1.
	// A server that doesn't know the version answers with version 1 and closes:
	// connect again one step lower.
	bool                         want_ext = shared_memory_wanted(opts);
	bool                         got_shm  = false;
	uint8_t                      offer[shm_offer_size];
	std::unique_ptr<shm_channel> shm;
	for (bool want_pair = pair_wanted();; want_ext ? (void)(want_ext = false) : (void)(want_pair = false)) {
		for (;;) {
			// SECURITY_IDENTIFICATION: the server may identify the client but never act
			// with its token (blocks named-pipe impersonation attacks).
			h = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
							FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
			if (h != INVALID_HANDLE_VALUE)
				break;
			DWORD err = GetLastError();
			if (err != ERROR_PIPE_BUSY)
				return map_error(err);
			DWORD left = remaining_ms();
			if (left == 0)
				return datapath::error::TimedOut;
			WaitNamedPipeW(wpath.c_str(), left);
		}

		// Handshake with event-based overlapped I/O before the handle joins the port.
		HANDLE ev       = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		auto   transfer = [&](bool write, uint8_t* buf, DWORD size) -> datapath::error {
	        DWORD total = 0;
	        while (total < size) {
	            OVERLAPPED ov{};
	            ov.hEvent = ev;
	            ResetEvent(ev);
	            DWORD got = 0;
	            BOOL  ok  = write ? WriteFile(h, buf + total, size - total, &got, &ov)
	                              : ReadFile(h, buf + total, size - total, &got, &ov);
	            if (!ok) {
	                DWORD err = GetLastError();
	                if (err != ERROR_IO_PENDING)
	                    return map_error(err);
	                if (WaitForSingleObject(ev, remaining_ms()) != WAIT_OBJECT_0) {
	                    CancelIoEx(h, &ov);
	                    GetOverlappedResult(h, &ov, &got, TRUE);
	                    return datapath::error::TimedOut;
	                }
	                if (!GetOverlappedResult(h, &ov, &got, FALSE))
	                    return map_error(GetLastError());
	            }
	            if (got == 0)
	                return datapath::error::Closed;
	            total += got;
	        }
	        return datapath::error::Success;
		};

		// With pair_peer, ask for version 2 (core pairing). A server that answers
		// version 1 doesn't know it and closes this connection: connect again.
		uint8_t  hello[datapath::detail::hello_size + datapath::detail::ext_trailer_size];
		uint32_t version = want_ext    ? datapath::detail::protocol_version_ext
						   : want_pair ? datapath::detail::protocol_version_pair
									   : datapath::detail::protocol_version;
		datapath::detail::encode_hello(hello, {version, opts.max_message_size});
		DWORD hello_out = DWORD(datapath::detail::hello_size);
		if (want_ext) {
			datapath::detail::encode_ext_trailer(hello + datapath::detail::hello_size,
												 datapath::detail::ext_shared_memory
													 | (want_pair ? datapath::detail::ext_pair : 0),
												 0);
			hello_out += DWORD(datapath::detail::ext_trailer_size);
		}
		datapath::error ec = transfer(true, hello, hello_out);
		if (ec == datapath::error::Success)
			ec = transfer(false, hello, DWORD(datapath::detail::hello_size));
		if (ec == datapath::error::Success)
			ec = want_ext ? datapath::detail::decode_hello_any(hello, reply)
				 : want_pair ? datapath::detail::decode_hello_v2(hello, reply)
							 : datapath::detail::decode_hello(hello, reply);
		bool fallback = false;
		if (ec == datapath::error::Success && want_ext) {
			if (reply.version == datapath::detail::protocol_version_ext) {
				uint8_t trailer[datapath::detail::ext_trailer_size];
				ec = transfer(false, trailer, sizeof(trailer));
				uint32_t cpu = 0, accepted = 0;
				if (ec == datapath::error::Success) {
					datapath::detail::decode_ext_trailer(trailer, cpu, accepted);
					pair_cpu = (accepted & datapath::detail::ext_pair) ? int32_t(cpu) : -1;
					got_shm  = (accepted & datapath::detail::ext_shared_memory) != 0;
				}
				if (ec == datapath::error::Success && got_shm)
					ec = transfer(false, offer, sizeof(offer));
			} else if (reply.version == datapath::detail::protocol_version) {
				fallback = true;
			} else {
				ec = datapath::error::VersionMismatch;
			}
		} else if (ec == datapath::error::Success && want_pair) {
			if (reply.version == datapath::detail::protocol_version_pair) {
				uint8_t trailer[datapath::detail::pair_trailer_size];
				ec = transfer(false, trailer, sizeof(trailer));
				if (ec == datapath::error::Success)
					pair_cpu = datapath::detail::decode_pair_trailer(trailer);
			} else {
				fallback = true;
			}
		}
		CloseHandle(ev);
		if (fallback) {
			CloseHandle(h);
			continue;
		}
		if (ec != datapath::error::Success) {
			CloseHandle(h);
			return ec;
		}
		break;
	}

	auto limit = std::min(opts.max_message_size, reply.max_message_size);
	if (got_shm) {
		auto ec = shm_channel::open(shm, offer, limit, opts.max_queued_bytes);
		if (ec != datapath::error::Success) {
			CloseHandle(h);
			return ec;
		}
	}
	bool skip  = associate(h);
	auto conn  = std::make_shared<pipe_connection>(shared_from_this(), h, skip, opts, limit);
	if (shm)
		conn->use_shared_memory(std::move(shm));
	if (pair_cpu >= 0 && pair_client(pair_cpu))
		conn->set_paired();
	add_member(std::weak_ptr<datapath::detail::connection>(conn));
	out = std::move(conn);
	return datapath::error::Success;
}

datapath::error datapath::windows::iocp_context::listen(std::shared_ptr<datapath::detail::listener>& out,
														const std::string& path, const datapath::options& opts)
{
	if (stopped())
		return datapath::error::Closed;
	std::wstring wpath;
	if (!make_pipe_path(path, wpath))
		return datapath::error::InvalidPath;
	auto lst = std::make_shared<pipe_listener>(shared_from_this(), std::move(wpath), opts);
	auto ec  = lst->create();
	if (ec != datapath::error::Success) {
		lst->close();
		return ec;
	}
	add_member(std::weak_ptr<datapath::detail::listener>(lst));
	out = std::move(lst);
	return datapath::error::Success;
}

std::shared_ptr<datapath::detail::context_impl> datapath::detail::make_platform_context(
	const datapath::context_options& options)
{
	auto ctx = std::make_shared<datapath::windows::iocp_context>(options);
	if (!ctx->start())
		return nullptr;
	return ctx;
}
