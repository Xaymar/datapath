/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "common/detail.hpp"
#include "win.hpp"

namespace datapath::windows {
	class io_target;

	// One outstanding overlapped operation. `keep` holds the target alive until
	// the completion has been dispatched; it is the library's internal reference.
	struct io_op : OVERLAPPED {
		std::shared_ptr<io_target> keep;
		int                        kind = 0;

		io_op()
		{
			reset();
		}
		void reset() noexcept
		{
			OVERLAPPED* o = this;
			ZeroMemory(o, sizeof(OVERLAPPED));
		}
	};

	class io_target {
		public:
		virtual ~io_target() = default;
		// Runs on an I/O thread. `err` is a Win32 error code (0 on success).
		virtual void on_io(io_op* op, DWORD bytes, DWORD err) = 0;
	};

	// Something the I/O threads busy-poll besides the port (shared-memory rings).
	// Each target is polled by the thread that owns it; a thread that handles
	// its wake-up packet takes it over.
	class poll_target {
		public:
		virtual ~poll_target() = default;
		// Handle new work; true if there was some.
		virtual bool poll() = 0;
		// About to block: arm the wake-up. False if work arrived meanwhile.
		virtual bool prepare_block() = 0;
		std::atomic<int> owner{0};
	};

	class iocp_context : public datapath::detail::context_impl, public std::enable_shared_from_this<iocp_context> {
		HANDLE                          _port = nullptr;
		datapath::context_options       _opts;
		std::vector<std::thread>        _threads;
		std::atomic<bool>               _stopped{false};
		std::atomic<int64_t>            _outstanding{0};
		std::mutex                      _wait_lock;
		std::condition_variable         _wait_cv;

		std::mutex                                       _poll_lock;
		std::vector<std::shared_ptr<poll_target>>        _pollers;
		std::atomic<uint64_t>                            _poll_version{0};

		std::mutex                                            _members_lock;
		std::vector<std::weak_ptr<datapath::detail::connection>> _connections;
		std::vector<std::weak_ptr<datapath::detail::listener>>   _listeners;

		// Core pairing (pair_peer): the physical core thread 0 prefers, as a
		// group-0 mask (0 = unknown), and the affinity to go back to when the
		// pairing ends.
		KAFFINITY         _pair_core   = 0;
		DWORD_PTR         _restore_mask = 0;
		std::atomic<bool> _paired{false};

		void run();
		void dispatch(OVERLAPPED_ENTRY& entry);
		bool pin_io_thread(DWORD_PTR mask);
		void shutdown_now();
		bool on_io_thread() const noexcept;

		public:
		explicit iocp_context(const datapath::context_options& opts);
		~iocp_context() override;

		bool start();

		HANDLE port() const noexcept
		{
			return _port;
		}
		// Index of the calling I/O thread, or 0 elsewhere.
		static int thread_index() noexcept;
		void add_poller(std::shared_ptr<poll_target> t);
		void remove_poller(poll_target* t);
		// Whether connections with these options should use shared memory.
		static bool shared_memory_wanted(const datapath::options& opts) noexcept;

		// Associate a handle with the port and enable skip-on-success completions.
		bool associate(HANDLE h);

		// Bookkeeping for operations that will produce a completion packet.
		void op_started() noexcept
		{
			_outstanding.fetch_add(1, std::memory_order_relaxed);
		}
		void op_finished() noexcept;

		// Queue `op` for `target` as if it completed (used to hop onto an I/O thread).
		bool post(io_op& op, std::shared_ptr<io_target> target);

		bool stopped() const noexcept
		{
			return _stopped.load(std::memory_order_acquire);
		}
		bool yield_enabled() const noexcept
		{
			return _opts.yield;
		}

		// Core pairing. Server side: pin the I/O thread to one hyper-thread of its
		// core and return the logical processor (group 0) the client should take,
		// or -1. Client side: pin the I/O thread to `cpu`; false if not possible.
		// unpair() undoes either when the paired connection finishes.
		int32_t pair_server();
		bool    pair_client(int32_t cpu);
		void    unpair();
		bool    pair_wanted() const noexcept
		{
			return _opts.pair_peer && _opts.io_threads == 1 && !_paired.load(std::memory_order_acquire);
		}

		void add_member(std::weak_ptr<datapath::detail::connection> c);
		void add_member(std::weak_ptr<datapath::detail::listener> l);

		datapath::error connect(std::shared_ptr<datapath::detail::connection>& out, const std::string& path,
								const datapath::options& options) override;
		datapath::error listen(std::shared_ptr<datapath::detail::listener>& out, const std::string& path,
							   const datapath::options& options) override;
		void            shutdown() override;
	};
} // namespace datapath::windows
