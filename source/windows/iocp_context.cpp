/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "iocp_context.hpp"
#include <algorithm>
#include <chrono>
#include "common/handshake.hpp"
#include "common/spin.hpp"
#include "path.hpp"
#include "pipe_connection.hpp"
#include "pipe_server.hpp"

#ifndef FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#endif
#ifndef FILE_SKIP_SET_EVENT_ON_HANDLE
#define FILE_SKIP_SET_EVENT_ON_HANDLE 0x2
#endif

namespace {
	// The context whose I/O thread is running on this thread, if any.
	thread_local const datapath::windows::iocp_context* tl_context = nullptr;

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
	for (size_t i = 0; i < _opts.io_threads; i++)
		_threads.emplace_back([this, keep = shared_from_this()] {
			tl_context = keep.get();
			run();
		});
	return true;
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
	while (!quit) {
		auto block = [&] {
			if (!quit)
				pump(INFINITE);
		};
		if (_opts.yield)
			datapath::detail::spin_then_block(_opts.spin, [&] { return !quit && pump(0); }, block, [] { Sleep(0); });
		else
			datapath::detail::spin_then_block(_opts.spin, [&] { return !quit && pump(0); }, block);
	}
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

	HANDLE h;
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

	uint8_t hello[datapath::detail::hello_size];
	datapath::detail::encode_hello(hello, {datapath::detail::protocol_version, opts.max_message_size});
	datapath::error ec = transfer(true, hello, sizeof(hello));
	if (ec == datapath::error::Success)
		ec = transfer(false, hello, sizeof(hello));
	CloseHandle(ev);
	datapath::detail::hello reply{};
	if (ec == datapath::error::Success)
		ec = datapath::detail::decode_hello(hello, reply);
	if (ec != datapath::error::Success) {
		CloseHandle(h);
		return ec;
	}

	bool skip  = associate(h);
	auto limit = std::min(opts.max_message_size, reply.max_message_size);
	auto conn  = std::make_shared<pipe_connection>(shared_from_this(), h, skip, opts, limit);
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
