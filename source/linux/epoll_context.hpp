/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/detail.hpp"

namespace datapath::linux_ {
	// Anything registered with an event loop.
	class fd_target {
		public:
		virtual ~fd_target() = default;
		// epoll readiness for the target's fd. Runs on the target's loop thread.
		virtual void on_event(uint32_t events) = 0;
		// A posted notification. Runs on the target's loop thread.
		virtual void on_post(int kind) = 0;
	};

	class epoll_context;

	class event_loop {
		friend class epoll_context;

		struct post_item {
			std::shared_ptr<fd_target> target; // null = quit
			int                        kind;
		};

		epoll_context*         _ctx;
		int                    _ep  = -1;
		int                    _efd = -1;
		std::thread            _thread;
		std::mutex             _post_lock;
		std::vector<post_item> _posts;
		bool                   _signaled = false;
		bool                   _dead     = false; // thread joined: posts are refused

		std::mutex                                                 _reg_lock;
		std::unordered_map<fd_target*, std::shared_ptr<fd_target>> _registered;

		bool drain_posts(bool& quit);
		void run();

		public:
		explicit event_loop(epoll_context* ctx);
		~event_loop();
		bool start();

		// Register `fd` for `events` (edge-triggered). The loop keeps `target`
		// alive until unregister().
		bool add(int fd, uint32_t events, std::shared_ptr<fd_target> target);
		bool modify(int fd, uint32_t events, fd_target* target);
		void remove_fd(int fd);
		// Drop the loop's reference. Call on the loop thread.
		void unregister(fd_target* target);

		bool post(std::shared_ptr<fd_target> target, int kind);
	};

	class epoll_context : public datapath::detail::context_impl, public std::enable_shared_from_this<epoll_context> {
		datapath::context_options                _opts;
		std::vector<std::unique_ptr<event_loop>> _loops;
		std::atomic<size_t>                      _next{0};
		std::atomic<bool>                        _stopped{false};

		std::mutex                                               _members_lock;
		std::vector<std::weak_ptr<datapath::detail::connection>> _connections;
		std::vector<std::weak_ptr<datapath::detail::listener>>   _listeners;

		void shutdown_now();

		public:
		explicit epoll_context(const datapath::context_options& opts);
		~epoll_context() override;
		bool start();
		// True on any I/O thread of this context.
		bool on_io_thread() const noexcept;

		std::chrono::microseconds spin() const noexcept
		{
			return _opts.spin;
		}
		int priority() const noexcept
		{
			return _opts.priority;
		}
		bool yield_enabled() const noexcept
		{
			return _opts.yield;
		}
		bool stopped() const noexcept
		{
			return _stopped.load(std::memory_order_acquire);
		}
		// Round-robin loop for a new connection.
		event_loop& pick_loop();

		void add_member(std::weak_ptr<datapath::detail::connection> c);
		void add_member(std::weak_ptr<datapath::detail::listener> l);

		datapath::error connect(std::shared_ptr<datapath::detail::connection>& out, const std::string& path,
								const datapath::options& options) override;
		datapath::error listen(std::shared_ptr<datapath::detail::listener>& out, const std::string& path,
							   const datapath::options& options) override;
		void            shutdown() override;
	};
} // namespace datapath::linux_
