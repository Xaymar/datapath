/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// event_loop on kqueue, woken by an EVFILT_USER event (macOS, iOS, BSDs).
//
// ev::in and ev::out map to a read and a write filter, added with EV_CLEAR
// (edge triggered, like EPOLLET) while their bit is set and deleted when it is
// cleared. A newly added filter is evaluated at once, so a socket that is
// already writable reports ev::out as soon as it is enabled.

#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include "event_context.hpp"
#include "platform.hpp"

namespace {
	constexpr uintptr_t wake_ident = 1;

	// Changes that take `fd` from `old_events` to `events`. Returns the count.
	int filter_changes(struct kevent (&changes)[2], int fd, uint32_t old_events, uint32_t events, void* udata)
	{
		int n = 0;
		for (auto [bit, filter] : {std::pair<uint32_t, short>{datapath::posix::ev::in, EVFILT_READ},
								   std::pair<uint32_t, short>{datapath::posix::ev::out, EVFILT_WRITE}}) {
			bool was = (old_events & bit) != 0, is = (events & bit) != 0;
			if (was != is)
				EV_SET(&changes[n++], uintptr_t(fd), filter, is ? (EV_ADD | EV_CLEAR) : EV_DELETE, 0, 0, udata);
		}
		return n;
	}
} // namespace

bool datapath::posix::event_loop::open_poller()
{
	_poll = ::kqueue();
	if (_poll < 0)
		return false;
	::fcntl(_poll, F_SETFD, FD_CLOEXEC);
	struct kevent ev;
	EV_SET(&ev, wake_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
	return ::kevent(_poll, &ev, 1, nullptr, 0, nullptr) == 0;
}

void datapath::posix::event_loop::close_poller()
{
	if (_poll >= 0)
		::close(_poll);
	_poll = -1;
}

bool datapath::posix::event_loop::add(int fd, uint32_t events, std::shared_ptr<fd_target> target)
{
	{
		std::lock_guard<std::mutex> l(_reg_lock);
		_registered[target.get()] = target;
	}
	if (!modify(fd, 0, events, target.get())) {
		remove_fd(fd);
		std::lock_guard<std::mutex> l(_reg_lock);
		_registered.erase(target.get());
		return false;
	}
	return true;
}

bool datapath::posix::event_loop::modify(int fd, uint32_t old_events, uint32_t events, fd_target* target)
{
	struct kevent changes[2];
	int           n = filter_changes(changes, fd, old_events, events, target);
	return n == 0 || ::kevent(_poll, changes, n, nullptr, 0, nullptr) == 0;
}

void datapath::posix::event_loop::remove_fd(int fd)
{
	// EV_RECEIPT reports each change separately, so a missing filter does not
	// stop the other one from being deleted.
	struct kevent changes[2], results[2];
	EV_SET(&changes[0], uintptr_t(fd), EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
	EV_SET(&changes[1], uintptr_t(fd), EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
	timespec zero{0, 0};
	::kevent(_poll, changes, 2, results, 2, &zero);
}

void datapath::posix::event_loop::wake()
{
	struct kevent ev;
	EV_SET(&ev, wake_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
	::kevent(_poll, &ev, 1, nullptr, 0, nullptr);
}

bool datapath::posix::event_loop::pump(int timeout, bool& quit)
{
	struct kevent events[64];
	timespec      ts{timeout / 1000, long(timeout % 1000) * 1000000};
	int           n = ::kevent(_poll, nullptr, 0, events, 64, timeout < 0 ? nullptr : &ts);
	if (n <= 0)
		return false;
	bool posts = false;
	for (int i = 0; i < n; i++) {
		auto& e = events[i];
		if (e.filter == EVFILT_USER) {
			posts = true;
			continue;
		}
		auto* t = static_cast<fd_target*>(e.udata);
		if (!t)
			continue;
		uint32_t bits = 0;
		if (e.flags & EV_ERROR)
			bits |= ev::err;
		else if (e.filter == EVFILT_READ)
			bits |= ev::in | ((e.flags & EV_EOF) ? ev::rdhup : 0u);
		else if (e.filter == EVFILT_WRITE)
			bits |= ev::out | ((e.flags & EV_EOF) ? ev::hup : 0u);
		t->on_event(bits);
	}
	if (posts)
		drain_posts(quit);
	return true;
}
