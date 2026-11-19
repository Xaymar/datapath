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

	class iocp_context : public datapath::detail::context_impl, public std::enable_shared_from_this<iocp_context> {
		HANDLE                          _port = nullptr;
		datapath::context_options       _opts;
		std::vector<std::thread>        _threads;
		std::atomic<bool>               _stopped{false};
		std::atomic<int64_t>            _outstanding{0};
		std::mutex                      _wait_lock;
		std::condition_variable         _wait_cv;

		std::mutex                                            _members_lock;
		std::vector<std::weak_ptr<datapath::detail::connection>> _connections;
		std::vector<std::weak_ptr<datapath::detail::listener>>   _listeners;

		void run();
		void dispatch(OVERLAPPED_ENTRY& entry);
		void shutdown_now();
		bool on_io_thread() const noexcept;

		public:
		explicit iocp_context(const datapath::context_options& opts);
		~iocp_context() override;

		bool start();

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

		void add_member(std::weak_ptr<datapath::detail::connection> c);
		void add_member(std::weak_ptr<datapath::detail::listener> l);

		datapath::error connect(std::shared_ptr<datapath::detail::connection>& out, const std::string& path,
								const datapath::options& options) override;
		datapath::error listen(std::shared_ptr<datapath::detail::listener>& out, const std::string& path,
							   const datapath::options& options) override;
		void            shutdown() override;
	};
} // namespace datapath::windows
