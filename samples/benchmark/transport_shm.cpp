/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// PROTOTYPE: Windows shared-memory transport, measured with the common harness.
//
// The named pipe only carries the handshake and then stays open as the liveness
// channel. Data moves through two single-producer/single-consumer byte rings in
// one section (one ring per direction). A receiver busy-polls its ring for the
// spin budget; only after that does it flag itself as waiting and block on an
// auto-reset event, and only then does the sender pay for SetEvent.
//
// Handshake (byte-mode pipe, both messages 64 bytes):
//   client -> server  "DPSM" u32 version, zero padding
//   server -> client  "DPSM" u32 version, u64 ring capacity, 32 hex chars of a
//                     random object name, zero padding
// The server then creates (never opens) the section and four events under
// "Local\dpshm-<name>-<suffix>". Each object's DACL grants full access to the
// server's user and only map/signal rights to the client's user, which the
// server learns by impersonating the pipe client at identification level. The
// name is 128 random bits and creation fails if the name exists, so another
// process can neither pre-create (squat) nor guess the objects.
//
// Peer crash: the pipe handle closes with the process, so the pending one-byte
// read on the pipe fails and wakes the receiver, which closes the connection.
// Nothing that the peer leaves in shared memory is trusted: each side keeps its
// own copy of its ring position, checks every record against the ring bounds,
// and copies a message out before delivery when the peer is another user (so
// the peer cannot change it while the handler reads it).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "bench.hpp"
#include "common/spin.hpp"
#include "windows/path.hpp"
#include "windows/security.hpp"
#include "windows/win.hpp"
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

namespace {
	constexpr uint32_t shm_version   = 1;
	constexpr size_t   hs_size       = 64;
	constexpr uint64_t rec_more      = uint64_t(1) << 32; // more fragments of this message follow
	constexpr uint64_t rec_pad       = uint64_t(1) << 33; // skip to the start of the ring
	constexpr uint64_t max_message   = uint64_t(1) << 30;
	constexpr DWORD    hs_timeout_ms = 5000;

	struct alignas(64) ring_ctl {
		alignas(64) std::atomic<uint64_t> head;           // bytes published by the writer
		alignas(64) std::atomic<uint64_t> tail;           // bytes released by the reader
		alignas(64) std::atomic<uint32_t> reader_waiting; // reader is (about to be) blocked on its event
		alignas(64) std::atomic<uint32_t> writer_waiting; // writer is (about to be) blocked for space
	};

	struct section_header {
		uint32_t magic;
		uint32_t version;
		uint64_t capacity;
		ring_ctl ring[2]; // [0] server -> client, [1] client -> server
	};
	static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared atomics must be lock-free");

	constexpr size_t data_offset = (sizeof(section_header) + 4095) & ~size_t(4095);

	uint64_t align8(uint64_t v)
	{
		return (v + 7) & ~uint64_t(7);
	}

	void disable_power_throttling()
	{
		struct throttling_state {
			ULONG Version, ControlMask, StateMask;
		};
		using fn_t     = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
		static auto fn = reinterpret_cast<fn_t>(
			reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation")));
		if (fn) {
			throttling_state s{1, 1, 0};
			fn(GetCurrentThread(), 3 /*ThreadPowerThrottling*/, &s, sizeof(s));
		}
	}

	std::wstring sid_string(PSID sid)
	{
		LPWSTR       str = nullptr;
		std::wstring out;
		if (ConvertSidToStringSidW(sid, &str)) {
			out = str;
			LocalFree(str);
		}
		return out;
	}

	// Overlapped transfer of exactly `size` bytes within `timeout_ms`.
	bool transfer(HANDLE h, HANDLE ev, bool write, uint8_t* buf, DWORD size, DWORD timeout_ms)
	{
		DWORD total = 0;
		while (total < size) {
			OVERLAPPED ov{};
			ov.hEvent = ev;
			ResetEvent(ev);
			DWORD got = 0;
			BOOL  ok  = write ? WriteFile(h, buf + total, size - total, &got, &ov)
							  : ReadFile(h, buf + total, size - total, &got, &ov);
			if (!ok) {
				if (GetLastError() != ERROR_IO_PENDING)
					return false;
				if (WaitForSingleObject(ev, timeout_ms) != WAIT_OBJECT_0) {
					CancelIoEx(h, &ov);
					GetOverlappedResult(h, &ov, &got, TRUE);
					return false;
				}
				if (!GetOverlappedResult(h, &ov, &got, FALSE))
					return false;
			}
			if (got == 0)
				return false;
			total += got;
		}
		return true;
	}

	struct shm_objects {
		HANDLE section    = nullptr;
		HANDLE events[4]  = {}; // data[0], space[0], data[1], space[1]
		void*  view       = nullptr;
		bool   copy       = true; // peer is another user: copy messages out before delivery

		~shm_objects()
		{
			if (view)
				UnmapViewOfFile(view);
			if (section)
				CloseHandle(section);
			for (HANDLE e : events)
				if (e)
					CloseHandle(e);
		}
	};

	const wchar_t* const suffixes[5] = {L"-m", L"-d0", L"-s0", L"-d1", L"-s1"};

	class shm_connection : public bench::connection {
		bench::config                _cfg;
		HANDLE                       _pipe;
		std::unique_ptr<shm_objects> _o;
		section_header*              _hdr;
		uint8_t*                     _data[2];
		uint64_t                     _cap;
		int                          _tx, _rx; // ring indices
		HANDLE                       _stop;    // process-local: wakes our own threads on close

		std::mutex  _send_lock;
		uint64_t    _head = 0; // writer's own copy, never re-read from shared memory
		uint64_t    _tail = 0; // reader's own copy
		std::thread _reader;
		std::atomic<bool> _started{false};
		std::atomic<bool> _closed{false};
		bench::message_fn _fn;
		std::function<void()> _close_fn;
		std::vector<uint8_t> _assembly, _scratch;

		ring_ctl& tx() const
		{
			return _hdr->ring[_tx];
		}
		ring_ctl& rx() const
		{
			return _hdr->ring[_rx];
		}
		HANDLE data_event(int ring) const
		{
			return _o->events[ring * 2];
		}
		HANDLE space_event(int ring) const
		{
			return _o->events[ring * 2 + 1];
		}

		// The connection ended on its own. `why` is printed for protocol errors
		// only; a peer that closed or died is reported through on_close.
		void fail(const char* why, bool report = true)
		{
			if (!_closed.exchange(true)) {
				if (report)
					std::fprintf(stderr, "shm: connection closed: %s\n", why);
				SetEvent(_stop);
				if (_close_fn)
					_close_fn();
			}
		}

		// Free bytes in the tx ring, or false when the peer's tail is impossible.
		bool free_space(uint64_t& out)
		{
			uint64_t used = _head - tx().tail.load(std::memory_order_acquire);
			if (used > _cap) {
				fail("peer wrote an invalid ring position");
				return false;
			}
			out = _cap - used;
			return true;
		}

		// Bytes needed at the current position for a record of `len` payload bytes.
		uint64_t needed(uint64_t len) const
		{
			uint64_t need   = 8 + align8(len);
			uint64_t to_end = _cap - (_head & (_cap - 1));
			return to_end < need ? to_end + need : need;
		}

		void wake_reader()
		{
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (tx().reader_waiting.load(std::memory_order_relaxed) && tx().reader_waiting.exchange(0))
				SetEvent(data_event(_tx));
		}

		void put(const uint8_t* p, uint64_t len, uint64_t flags)
		{
			uint64_t need = 8 + align8(len);
			uint64_t pos  = _head & (_cap - 1);
			uint8_t* d    = _data[_tx];
			if (_cap - pos < need) {
				uint64_t pad = rec_pad;
				std::memcpy(d + pos, &pad, 8);
				_head += _cap - pos;
				pos = 0;
			}
			uint64_t h = len | flags;
			std::memcpy(d + pos, &h, 8);
			std::memcpy(d + pos + 8, p, size_t(len));
			_head += need;
			tx().head.store(_head, std::memory_order_release);
			wake_reader();
		}

		// Large messages only: wait until `need` bytes are free.
		bool wait_space(uint64_t need)
		{
			using clock   = std::chrono::steady_clock;
			auto deadline = clock::now() + std::chrono::microseconds(_cfg.spin);
			for (;;) {
				uint64_t f;
				if (_closed.load(std::memory_order_acquire) || !free_space(f))
					return false;
				if (f >= need)
					return true;
				if (clock::now() < deadline) {
					DATAPATH_CPU_RELAX();
					continue;
				}
				tx().writer_waiting.store(1, std::memory_order_seq_cst);
				if (!free_space(f))
					return false;
				if (f >= need) {
					tx().writer_waiting.store(0, std::memory_order_relaxed);
					return true;
				}
				HANDLE hs[2] = {space_event(_tx), _stop};
				WaitForMultipleObjects(2, hs, FALSE, 100);
			}
		}

		// ---------------------------------------------------------------- receive

		bool deliver(const uint8_t* p, uint64_t len, bool more)
		{
			if (more || !_assembly.empty()) {
				if (_assembly.size() + len > max_message) {
					fail("peer sent an oversized message");
					return false;
				}
				_assembly.insert(_assembly.end(), p, p + len);
				if (more)
					return true;
				_fn(_assembly.data(), _assembly.size());
				_assembly.clear();
				return true;
			}
			if (_o->copy) {
				_scratch.assign(p, p + len);
				p = _scratch.data();
			}
			_fn(p, size_t(len));
			return true;
		}

		// Handle everything published so far. Returns true when there was work.
		bool poll()
		{
			uint64_t h = rx().head.load(std::memory_order_acquire);
			if (h == _tail)
				return false;
			if (h - _tail > _cap) {
				fail("peer published an invalid ring position");
				return false;
			}
			const uint8_t* d = _data[_rx];
			while (_tail != h) {
				uint64_t pos = _tail & (_cap - 1);
				uint64_t rec;
				std::memcpy(&rec, d + pos, 8); // read once: the peer may change it
				uint64_t step;
				if (rec & rec_pad) {
					step = _cap - pos;
					if (pos == 0 || step > h - _tail) {
						fail("peer wrote an invalid padding record");
						return false;
					}
				} else {
					uint64_t len = rec & 0xffffffffu;
					step         = 8 + align8(len);
					if (step > _cap - pos || step > h - _tail || len > _cap / 2) {
						fail("peer wrote an invalid record");
						return false;
					}
					if (!deliver(d + pos + 8, len, (rec & rec_more) != 0))
						return false;
				}
				_tail += step;
				rx().tail.store(_tail, std::memory_order_release);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				if (rx().writer_waiting.load(std::memory_order_relaxed) && rx().writer_waiting.exchange(0))
					SetEvent(space_event(_rx));
				if (_closed.load(std::memory_order_relaxed))
					return false;
			}
			return true;
		}

		void reader_main()
		{
			disable_power_throttling();
			bench::tune_io_thread(_cfg);
			HANDLE     pipe_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			OVERLAPPED ov{};
			ov.hEvent     = pipe_ev;
			uint8_t probe = 0;
			// The peer never writes to the pipe after the handshake: this read only
			// completes when the peer closes it or its process ends.
			if (!pipe_ev || ReadFile(_pipe, &probe, 1, nullptr, &ov) || GetLastError() != ERROR_IO_PENDING) {
				fail("pipe closed", false);
			}
			while (!_closed.load(std::memory_order_acquire)) {
				datapath::detail::spin_then_block(
					std::chrono::microseconds(_cfg.spin),
					[&] { return !_closed.load(std::memory_order_relaxed) && poll(); },
					[&] {
						if (_closed.load(std::memory_order_relaxed))
							return;
						rx().reader_waiting.store(1, std::memory_order_seq_cst);
						if (rx().head.load(std::memory_order_seq_cst) != _tail) {
							rx().reader_waiting.store(0, std::memory_order_relaxed);
							return;
						}
						HANDLE hs[3] = {data_event(_rx), pipe_ev, _stop};
						DWORD  w     = WaitForMultipleObjects(3, hs, FALSE, INFINITE);
						rx().reader_waiting.store(0, std::memory_order_relaxed);
						if (w == WAIT_OBJECT_0 + 1)
							fail("peer closed the pipe", false);
					});
			}
			CancelIoEx(_pipe, &ov);
			DWORD got = 0;
			GetOverlappedResult(_pipe, &ov, &got, TRUE);
			if (pipe_ev)
				CloseHandle(pipe_ev);
		}

		public:
		shm_connection(const bench::config& cfg, HANDLE pipe, std::unique_ptr<shm_objects> o, bool server)
			: _cfg(cfg), _pipe(pipe), _o(std::move(o))
		{
			_hdr     = static_cast<section_header*>(_o->view);
			_cap     = _hdr->capacity;
			_data[0] = static_cast<uint8_t*>(_o->view) + data_offset;
			_data[1] = _data[0] + _cap;
			_tx      = server ? 0 : 1;
			_rx      = server ? 1 : 0;
			_stop    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		}

		~shm_connection() override
		{
			_closed = true;
			SetEvent(_stop);
			if (_reader.joinable()) {
				if (_reader.get_id() == std::this_thread::get_id())
					_reader.detach();
				else
					_reader.join();
			}
			// Closing the pipe tells the peer we are gone.
			CloseHandle(_pipe);
			CloseHandle(_stop);
		}

		bench::status send(const void* data, size_t size) override
		{
			std::lock_guard<std::mutex> l(_send_lock);
			if (_closed.load(std::memory_order_acquire))
				return bench::status::closed;
			if (size > max_message)
				return bench::status::failed;
			auto*    p        = static_cast<const uint8_t*>(data);
			uint64_t frag_max = _cap / 2 - 8;
			uint64_t f;
			if (size <= frag_max) {
				// Never blocks: like a full send queue, a full ring is WouldBlock.
				if (!free_space(f))
					return bench::status::closed;
				if (f < needed(size))
					return bench::status::would_block;
				put(p, size, 0);
				return bench::status::ok;
			}
			// Larger than half the ring: stream it in fragments as the reader frees
			// space. (Two such messages crossing on the same connection from both
			// readers' handlers would deadlock; fine for the benchmark's ping-pong.)
			while (size) {
				uint64_t n = std::min<uint64_t>(size, frag_max);
				if (!wait_space(needed(n)))
					return bench::status::closed;
				put(p, n, size > n ? rec_more : 0);
				p += n;
				size -= size_t(n);
			}
			return bench::status::ok;
		}

		void on_close(std::function<void()> fn) override
		{
			if (!_started.load())
				_close_fn = std::move(fn);
		}

		void on_message(bench::message_fn fn) override
		{
			if (_started.exchange(true))
				return; // the harness sets the handler once per connection
			_fn     = std::move(fn);
			_reader = std::thread([this] { reader_main(); });
		}
	};

	// ---------------------------------------------------------------- transport

	class shm_transport : public bench::transport {
		bench::config _cfg;
		std::wstring  _path;
		std::thread   _acceptor;
		HANDLE        _stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		PSECURITY_DESCRIPTOR _pipe_sd = nullptr;
		std::wstring         _own_sid = datapath::windows::current_user_sid();

		uint64_t ring_capacity() const
		{
			// --pipe-buffer sets the ring size per direction (rounded up to a power
			// of two, at least 64 KiB); the default 1 MiB pipe buffer gives 4 MiB
			// rings so the benchmark's sizes up to 1 MiB are never fragmented.
			uint64_t want = _cfg.pipe_buffer == datapath::options{}.pipe_buffer ? (uint64_t(4) << 20) : _cfg.pipe_buffer;
			uint64_t cap  = uint64_t(64) << 10;
			while (cap < want && cap < (uint64_t(1) << 30))
				cap <<= 1;
			return cap;
		}

		// The pipe client's user SID, read at identification level.
		std::wstring client_sid(HANDLE pipe)
		{
			if (!ImpersonateNamedPipeClient(pipe))
				return {};
			HANDLE       token = nullptr;
			std::wstring out;
			if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
				DWORD size = 0;
				GetTokenInformation(token, TokenUser, nullptr, 0, &size);
				std::vector<uint8_t> buf(size ? size : 1);
				if (GetTokenInformation(token, TokenUser, buf.data(), size, &size))
					out = sid_string(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid);
				CloseHandle(token);
			}
			RevertToSelf();
			return out;
		}

		std::unique_ptr<shm_objects> create_objects(const std::wstring& base, const std::wstring& peer_sid, uint64_t cap)
		{
			auto o  = std::make_unique<shm_objects>();
			o->copy = peer_sid != _own_sid;
			// Owner: everything. Peer: map read/write + query + read control
			// (0x20007) for the section, modify state + synchronize (0x100002) for
			// the events. Nothing for anyone else.
			auto make_sa = [&](const wchar_t* peer_mask, PSECURITY_DESCRIPTOR& sd) {
				std::wstring sddl = L"D:P(A;;GA;;;" + _own_sid + L")(A;;" + peer_mask + L";;;" + peer_sid + L")";
				return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)
					   != FALSE;
			};
			PSECURITY_DESCRIPTOR sd_sec = nullptr, sd_ev = nullptr;
			bool                 ok     = make_sa(L"0x20007", sd_sec) && make_sa(L"0x100002", sd_ev);
			SECURITY_ATTRIBUTES  sa_sec{sizeof(sa_sec), sd_sec, FALSE}, sa_ev{sizeof(sa_ev), sd_ev, FALSE};
			uint64_t             total = data_offset + 2 * cap;
			if (ok) {
				o->section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa_sec, PAGE_READWRITE | SEC_COMMIT,
												DWORD(total >> 32), DWORD(total), (base + suffixes[0]).c_str());
				ok         = o->section && GetLastError() != ERROR_ALREADY_EXISTS;
			}
			for (int i = 0; ok && i < 4; i++) {
				o->events[i] = CreateEventW(&sa_ev, FALSE, FALSE, (base + suffixes[i + 1]).c_str());
				ok           = o->events[i] && GetLastError() != ERROR_ALREADY_EXISTS;
			}
			if (ok)
				ok = (o->view = MapViewOfFile(o->section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0)) != nullptr;
			if (sd_sec)
				LocalFree(sd_sec);
			if (sd_ev)
				LocalFree(sd_ev);
			if (!ok)
				return nullptr;
			auto* hdr     = new (o->view) section_header{};
			hdr->magic    = 0x4d535044; // "DPSM"
			hdr->version  = shm_version;
			hdr->capacity = cap;
			return o;
		}

		// One client: handshake, objects, connection. False drops it.
		bool serve(HANDLE pipe, HANDLE ev, std::function<void(bench::conn_ptr)>& on_accept)
		{
			uint8_t buf[hs_size] = {};
			if (!transfer(pipe, ev, false, buf, hs_size, hs_timeout_ms) || std::memcmp(buf, "DPSM", 4) != 0)
				return false;
			uint32_t ver;
			std::memcpy(&ver, buf + 4, 4);
			std::wstring peer = client_sid(pipe);
			if (ver != shm_version || peer.empty() || _own_sid.empty())
				return false;

			uint8_t rnd[16];
			if (BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
				return false;
			char hex[33];
			for (int i = 0; i < 16; i++)
				std::snprintf(hex + 2 * i, 3, "%02x", rnd[i]);
			std::wstring base = L"Local\\dpshm-" + std::wstring(hex, hex + 32);
			uint64_t     cap  = ring_capacity();
			auto         o    = create_objects(base, peer, cap);
			if (!o)
				return false;

			std::memset(buf, 0, sizeof(buf));
			std::memcpy(buf, "DPSM", 4);
			std::memcpy(buf + 4, &shm_version, 4);
			std::memcpy(buf + 8, &cap, 8);
			std::memcpy(buf + 16, hex, 32);
			if (!transfer(pipe, ev, true, buf, hs_size, hs_timeout_ms))
				return false;
			on_accept(std::make_shared<shm_connection>(_cfg, pipe, std::move(o), true));
			return true;
		}

		void accept_main(std::function<void(bench::conn_ptr)> on_accept)
		{
			HANDLE ev    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			bool   first = true;
			SECURITY_ATTRIBUTES sa{sizeof(sa), _pipe_sd, FALSE};
			while (WaitForSingleObject(_stop, 0) != WAIT_OBJECT_0) {
				DWORD  mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
				HANDLE pipe = CreateNamedPipeW(_path.c_str(), mode,
											   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
											   PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &sa);
				first = false;
				if (pipe == INVALID_HANDLE_VALUE) {
					std::fprintf(stderr, "shm: CreateNamedPipe failed (%lu)\n", GetLastError());
					return;
				}
				OVERLAPPED ov{};
				ov.hEvent = ev;
				ResetEvent(ev);
				bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
				DWORD err      = connected ? ERROR_PIPE_CONNECTED : GetLastError();
				if (err == ERROR_IO_PENDING) {
					HANDLE hs[2] = {ev, _stop};
					if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) {
						CancelIoEx(pipe, &ov);
						CloseHandle(pipe);
						return;
					}
					DWORD got = 0;
					err       = GetOverlappedResult(pipe, &ov, &got, FALSE) ? ERROR_PIPE_CONNECTED : GetLastError();
				}
				if (err != ERROR_PIPE_CONNECTED || !serve(pipe, ev, on_accept))
					CloseHandle(pipe);
			}
			CloseHandle(ev);
		}

		public:
		explicit shm_transport(const bench::config& c) : _cfg(c) {}

		~shm_transport() override
		{
			SetEvent(_stop);
			if (_acceptor.joinable())
				_acceptor.join();
			if (_pipe_sd)
				LocalFree(_pipe_sd);
			CloseHandle(_stop);
		}

		const char* name() const override
		{
			return "shm-prototype";
		}

		bench::status host(const std::string& path, std::function<void(bench::conn_ptr)> on_accept) override
		{
			if (!datapath::windows::make_pipe_path(path, _path))
				return bench::status::failed;
			_pipe_sd = datapath::windows::make_security_descriptor(datapath::permissions::User);
			if (!_pipe_sd)
				return bench::status::failed;
			_acceptor = std::thread([this, on_accept] { accept_main(on_accept); });
			return bench::status::ok;
		}

		bench::status connect(const std::string& path, bench::conn_ptr& out) override
		{
			std::wstring wpath;
			if (!datapath::windows::make_pipe_path(path, wpath))
				return bench::status::failed;
			HANDLE pipe = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
									  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
			if (pipe == INVALID_HANDLE_VALUE) {
				DWORD err = GetLastError();
				return err == ERROR_FILE_NOT_FOUND || err == ERROR_PIPE_BUSY ? bench::status::not_found
																			  : bench::status::failed;
			}
			HANDLE  ev           = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			uint8_t buf[hs_size] = {};
			std::memcpy(buf, "DPSM", 4);
			std::memcpy(buf + 4, &shm_version, 4);
			bool ok = ev && transfer(pipe, ev, true, buf, hs_size, hs_timeout_ms)
					  && transfer(pipe, ev, false, buf, hs_size, hs_timeout_ms);
			if (ev)
				CloseHandle(ev);
			uint32_t ver = 0;
			uint64_t cap = 0;
			std::memcpy(&ver, buf + 4, 4);
			std::memcpy(&cap, buf + 8, 8);
			ok = ok && std::memcmp(buf, "DPSM", 4) == 0 && ver == shm_version && cap >= (64u << 10)
				 && cap <= (uint64_t(1) << 30) && (cap & (cap - 1)) == 0;
			for (int i = 16; ok && i < 48; i++)
				ok = (buf[i] >= '0' && buf[i] <= '9') || (buf[i] >= 'a' && buf[i] <= 'f');
			auto o = std::make_unique<shm_objects>();
			if (ok) {
				std::wstring base = L"Local\\dpshm-" + std::wstring(buf + 16, buf + 48);
				o->section        = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (base + suffixes[0]).c_str());
				ok                = o->section != nullptr;
				for (int i = 0; ok && i < 4; i++)
					ok = (o->events[i] = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (base + suffixes[i + 1]).c_str()))
						 != nullptr;
			}
			if (ok) {
				// Copy messages out unless the section's owner is our own user.
				PSID                 owner = nullptr;
				PSECURITY_DESCRIPTOR sd    = nullptr;
				if (GetSecurityInfo(o->section, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
									nullptr, &sd)
					== ERROR_SUCCESS) {
					o->copy = sid_string(owner) != datapath::windows::current_user_sid();
					LocalFree(sd);
				}
				ok = (o->view = MapViewOfFile(o->section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0)) != nullptr;
			}
			if (ok) {
				// The server sized the section; check it really holds both rings
				// before touching anything past the header.
				MEMORY_BASIC_INFORMATION mbi{};
				auto*                    hdr = static_cast<section_header*>(o->view);
				ok = VirtualQuery(o->view, &mbi, sizeof(mbi)) && mbi.RegionSize >= data_offset + 2 * cap
					 && hdr->magic == 0x4d535044 && hdr->version == shm_version && hdr->capacity == cap;
			}
			if (!ok) {
				CloseHandle(pipe);
				return bench::status::failed;
			}
			out = std::make_shared<shm_connection>(_cfg, pipe, std::move(o), false);
			return bench::status::ok;
		}
	};
} // namespace

std::unique_ptr<bench::transport> bench::make_transport(const config& c)
{
	return std::make_unique<shm_transport>(c);
}
