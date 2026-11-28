/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Server-side handshake deadlines (options::handshake_timeout).
//
// One thread per context, started with the first deadline and blocked on a
// condition variable otherwise, so the I/O threads never look at a clock. A
// connection that is still waiting for the client's hello when its deadline
// passes is closed with TimedOut; one whose hello arrived in time is skipped.

#pragma once
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace datapath { namespace detail {
	class connection_base;

	class handshake_watchdog {
		public:
		using clock = std::chrono::steady_clock;

		handshake_watchdog() : _state(std::make_shared<state>()) {}
		~handshake_watchdog();
		handshake_watchdog(const handshake_watchdog&)            = delete;
		handshake_watchdog& operator=(const handshake_watchdog&) = delete;

		// Expire `conn`'s handshake at `deadline`. False if the watchdog is stopped
		// or its thread could not be started.
		bool watch(std::weak_ptr<connection_base> conn, clock::time_point deadline) noexcept;
		// Stop the thread and forget all deadlines. Idempotent; safe from the
		// watchdog's own thread (it then finishes on its own).
		void stop() noexcept;

		private:
		struct entry {
			clock::time_point              deadline;
			std::weak_ptr<connection_base> conn;
		};
		struct later {
			bool operator()(const entry& a, const entry& b) const noexcept
			{
				return a.deadline > b.deadline;
			}
		};
		// Shared with the thread, which may outlive the watchdog when it drops
		// the last reference to its own context.
		struct state {
			std::mutex                                            lock;
			std::condition_variable                               cv;
			std::priority_queue<entry, std::vector<entry>, later> queue;
			bool                                                  stopped = false;
		};

		std::shared_ptr<state> _state;
		std::thread            _thread; // guarded by _state->lock

		static void run(std::shared_ptr<state> s) noexcept;
	};
}} // namespace datapath::detail
