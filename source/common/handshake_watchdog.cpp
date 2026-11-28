/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "handshake_watchdog.hpp"
#include "connection_base.hpp"

datapath::detail::handshake_watchdog::~handshake_watchdog()
{
	stop();
}

bool datapath::detail::handshake_watchdog::watch(std::weak_ptr<connection_base> conn,
												 clock::time_point              deadline) noexcept
{
	try {
		std::lock_guard<std::mutex> l(_state->lock);
		if (_state->stopped)
			return false;
		if (!_thread.joinable())
			_thread = std::thread(&handshake_watchdog::run, _state);
		bool earliest = _state->queue.empty() || deadline < _state->queue.top().deadline;
		_state->queue.push(entry{deadline, std::move(conn)});
		if (earliest)
			_state->cv.notify_one();
		return true;
	} catch (...) {
		// No thread or no memory: the caller refuses the connection.
		return false;
	}
}

void datapath::detail::handshake_watchdog::stop() noexcept
{
	std::thread t;
	{
		std::lock_guard<std::mutex> l(_state->lock);
		_state->stopped = true;
		while (!_state->queue.empty())
			_state->queue.pop();
		t = std::move(_thread);
	}
	_state->cv.notify_all();
	if (!t.joinable())
		return;
	if (t.get_id() == std::this_thread::get_id())
		t.detach(); // it holds its own reference to the state and exits next
	else
		t.join();
}

void datapath::detail::handshake_watchdog::run(std::shared_ptr<state> s) noexcept
{
	std::unique_lock<std::mutex> l(s->lock);
	while (!s->stopped) {
		if (s->queue.empty()) {
			s->cv.wait(l);
			continue;
		}
		auto deadline = s->queue.top().deadline;
		if (clock::now() < deadline) {
			s->cv.wait_until(l, deadline);
			continue;
		}
		std::weak_ptr<connection_base> w = s->queue.top().conn;
		s->queue.pop();
		l.unlock();
		{
			// Unlocked: closing may drop the last reference to the connection
			// and, with it, to the context that owns this watchdog.
			try {
				if (auto c = w.lock())
					c->handshake_expired();
			} catch (...) {
				// Out of memory while closing: the connection stays as it is.
			}
		}
		w.reset();
		l.lock();
	}
}
