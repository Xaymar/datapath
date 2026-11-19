/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// event_loop on epoll, woken by an eventfd (Linux, Android).

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "event_context.hpp"
#include "platform.hpp"

namespace {
	uint32_t to_epoll(uint32_t events)
	{
		uint32_t r = EPOLLET;
		if (events & datapath::posix::ev::in)
			r |= EPOLLIN;
		if (events & datapath::posix::ev::out)
			r |= EPOLLOUT;
		if (events & datapath::posix::ev::rdhup)
			r |= EPOLLRDHUP;
		return r;
	}

	uint32_t from_epoll(uint32_t events)
	{
		uint32_t r = 0;
		if (events & EPOLLIN)
			r |= datapath::posix::ev::in;
		if (events & EPOLLOUT)
			r |= datapath::posix::ev::out;
		if (events & EPOLLRDHUP)
			r |= datapath::posix::ev::rdhup;
		if (events & EPOLLHUP)
			r |= datapath::posix::ev::hup;
		if (events & EPOLLERR)
			r |= datapath::posix::ev::err;
		return r;
	}
} // namespace

bool datapath::posix::event_loop::open_poller()
{
	_poll = ::epoll_create1(EPOLL_CLOEXEC);
	_efd  = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (_poll < 0 || _efd < 0)
		return false;
	epoll_event ev{};
	ev.events   = EPOLLIN;
	ev.data.ptr = nullptr;
	return ::epoll_ctl(_poll, EPOLL_CTL_ADD, _efd, &ev) == 0;
}

void datapath::posix::event_loop::close_poller()
{
	if (_efd >= 0)
		::close(_efd);
	if (_poll >= 0)
		::close(_poll);
	_efd = _poll = -1;
}

bool datapath::posix::event_loop::add(int fd, uint32_t events, std::shared_ptr<fd_target> target)
{
	{
		std::lock_guard<std::mutex> l(_reg_lock);
		_registered[target.get()] = target;
	}
	epoll_event ev{};
	ev.events   = to_epoll(events);
	ev.data.ptr = target.get();
	if (::epoll_ctl(_poll, EPOLL_CTL_ADD, fd, &ev) != 0) {
		std::lock_guard<std::mutex> l(_reg_lock);
		_registered.erase(target.get());
		return false;
	}
	return true;
}

bool datapath::posix::event_loop::modify(int fd, uint32_t, uint32_t events, fd_target* target)
{
	epoll_event ev{};
	ev.events   = to_epoll(events);
	ev.data.ptr = target;
	return ::epoll_ctl(_poll, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void datapath::posix::event_loop::remove_fd(int fd)
{
	::epoll_ctl(_poll, EPOLL_CTL_DEL, fd, nullptr);
}

void datapath::posix::event_loop::wake()
{
	uint64_t one = 1;
	ssize_t  r   = ::write(_efd, &one, sizeof(one));
	(void)r;
}

bool datapath::posix::event_loop::pump(int timeout, bool& quit)
{
	epoll_event events[64];
	int         n = ::epoll_wait(_poll, events, 64, timeout);
	if (n <= 0)
		return false;
	bool posts = false;
	for (int i = 0; i < n; i++) {
		auto* t = static_cast<fd_target*>(events[i].data.ptr);
		if (!t) {
			posts = true;
			continue;
		}
		t->on_event(from_epoll(events[i].events));
	}
	if (posts) {
		uint64_t v;
		ssize_t  r = ::read(_efd, &v, sizeof(v));
		(void)r;
		drain_posts(quit);
	}
	return true;
}
