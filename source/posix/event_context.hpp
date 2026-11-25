/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

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

namespace datapath { namespace posix {
	// Anything registered with an event loop.
	class fd_target {
		public:
		virtual ~fd_target() = default;
		// Readiness (ev:: bits) for the target's fd. Runs on the target's loop thread.
		virtual void on_event(uint32_t events) = 0;
		// A posted notification. Runs on the target's loop thread.
		virtual void on_post(int kind) = 0;
	};

	// Something a loop busy-polls besides its descriptors (shared-memory rings).
	class poll_target {
		public:
		virtual ~poll_target() = default;
		// Handle new work; true if there was some. Runs on the loop thread.
		virtual bool poll() = 0;
		// About to block: arm the wake-up. False if work arrived meanwhile.
		virtual bool prepare_block() = 0;
	};

	class event_context;

	class event_loop {
		friend class event_context;

		struct post_item {
			std::shared_ptr<fd_target> target; // null = quit
			int                        kind;
		};

		event_context*         _ctx;
		int                    _poll = -1; // epoll or kqueue descriptor
		int                    _efd  = -1; // epoll: eventfd that wakes the loop
		std::thread            _thread;
		std::mutex             _post_lock;
		std::vector<post_item> _posts;
		bool                   _signaled = false;
		bool                   _dead     = false; // thread joined: posts are refused

		std::mutex                                                 _reg_lock;
		std::unordered_map<fd_target*, std::shared_ptr<fd_target>> _registered;

		std::mutex                                _poll_lock;
		std::vector<std::shared_ptr<poll_target>> _pollers;
		std::atomic<uint64_t>                     _poll_version{0};

		bool drain_posts(bool& quit);
		void run();

		// Implemented by the poller backend (event_loop_epoll.cpp or
		// event_loop_kqueue.cpp).
		bool open_poller();
		void close_poller();
		void wake();
		// Wait up to `timeout` ms (-1 = forever, 0 = poll) and dispatch. False if
		// nothing happened.
		bool pump(int timeout, bool& quit);

		public:
		explicit event_loop(event_context* ctx);
		~event_loop();
		bool start();

		// Register `fd` for `events` (ev:: bits, edge-triggered). The loop keeps
		// `target` alive until unregister(). modify() changes the registration
		// from `old_events` to `events` and re-checks readiness, so enabling
		// ev::out on a socket that is already writable reports it.
		bool add(int fd, uint32_t events, std::shared_ptr<fd_target> target);
		bool modify(int fd, uint32_t old_events, uint32_t events, fd_target* target);
		void remove_fd(int fd);
		// Drop the loop's reference. Call on the loop thread.
		void unregister(fd_target* target);

		bool post(std::shared_ptr<fd_target> target, int kind);

		// Poll `t` along with the descriptors until remove_poller().
		void add_poller(std::shared_ptr<poll_target> t);
		void remove_poller(poll_target* t);
	};

	class event_context : public datapath::detail::context_impl, public std::enable_shared_from_this<event_context> {
		datapath::context_options                _opts;
		std::vector<std::unique_ptr<event_loop>> _loops;
		std::atomic<size_t>                      _next{0};
		std::atomic<bool>                        _stopped{false};

		std::mutex                                               _members_lock;
		std::vector<std::weak_ptr<datapath::detail::connection>> _connections;
		std::vector<std::weak_ptr<datapath::detail::listener>>   _listeners;

		void shutdown_now();

		public:
		explicit event_context(const datapath::context_options& opts);
		~event_context() override;
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
}} // namespace datapath::posix
