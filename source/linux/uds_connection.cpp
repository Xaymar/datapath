/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "uds_connection.hpp"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sched.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include "uds_server.hpp"

namespace {
	datapath::error map_errno(int e)
	{
		switch (e) {
		case EPIPE:
		case ECONNRESET:
		case ENOTCONN:
		case EBADF:
		case ESHUTDOWN:
			return datapath::error::Closed;
		default:
			return datapath::error::Failure;
		}
	}
} // namespace

datapath::linux_::uds_connection::uds_connection(std::shared_ptr<epoll_context> ctx, event_loop& loop, int fd,
												 const datapath::options& opts, uint64_t negotiated_limit)
	: connection_base(opts, negotiated_limit), _ctx(std::move(ctx)), _loop(loop), _fd(fd),
	  _mask(EPOLLRDHUP | EPOLLET)
{}

datapath::linux_::uds_connection::~uds_connection()
{
	if (_fd >= 0)
		::close(_fd);
}

bool datapath::linux_::uds_connection::start(bool handshake, std::weak_ptr<uds_listener> listener,
											 uint64_t own_limit)
{
	if (handshake) {
		_handshaking = true;
		_listener    = std::move(listener);
		_own_limit   = own_limit;
		_queue.try_acquire(); // the reply goes out before anything the user queues
		_mask |= EPOLLIN;
	}
	return _loop.add(_fd, _mask, self());
}

void datapath::linux_::uds_connection::update_mask(uint32_t add, uint32_t remove)
{
	std::shared_lock<std::shared_mutex> fl(_fd_lock);
	if (_fd < 0)
		return;
	std::lock_guard<std::mutex> l(_mask_lock);
	uint32_t                    mask = (_mask | add) & ~remove;
	if (mask == _mask)
		return;
	_mask = mask;
	_loop.modify(_fd, _mask, this);
}

void datapath::linux_::uds_connection::read_loop()
{
	for (;;) {
		if (_closed.load(std::memory_order_acquire)) {
			finish_close();
			return;
		}
		auto    area = _parser.prepare();
		ssize_t n;
		int     err = 0;
		{
			std::shared_lock<std::shared_mutex> fl(_fd_lock);
			if (_fd < 0) {
				n   = -1;
				err = EBADF;
			} else {
				n   = ::recv(_fd, area.first, area.second, MSG_DONTWAIT);
				err = errno;
			}
		}
		if (n > 0) {
			if (!consume(size_t(n)))
				return;
			continue;
		}
		if (n == 0) {
			close_with(datapath::error::Closed);
			finish_close();
			return;
		}
		if (err == EINTR)
			continue;
		if (err == EAGAIN || err == EWOULDBLOCK)
			return;
		close_with(map_errno(err));
		finish_close();
		return;
	}
}

void datapath::linux_::uds_connection::write_loop()
{
	for (;;) {
		if (_woff >= _wbatch.size()) {
			_woff = 0;
			if (!_queue.take(_wbatch)) {
				if (_close_when_flushed)
					close_with(datapath::error::VersionMismatch);
				return;
			}
		}
		ssize_t n;
		int     err = 0;
		{
			std::shared_lock<std::shared_mutex> fl(_fd_lock);
			if (_fd < 0)
				return;
			n   = ::send(_fd, _wbatch.data() + _woff, _wbatch.size() - _woff, MSG_DONTWAIT | MSG_NOSIGNAL);
			err = errno;
		}
		if (n > 0) {
			_woff += size_t(n);
			continue;
		}
		if (n < 0 && err == EINTR)
			continue;
		if (n < 0 && (err == EAGAIN || err == EWOULDBLOCK)) {
			// Hand the writer slot to the loop: it resumes on EPOLLOUT. Adding
			// EPOLLOUT via MOD re-checks readiness, so no wake-up is lost.
			_write_parked.store(true, std::memory_order_release);
			update_mask(EPOLLOUT, 0);
			return;
		}
		close_with(map_errno(err));
		return;
	}
}

void datapath::linux_::uds_connection::hello_read()
{
	while (_handshaking && !_closed.load(std::memory_order_acquire)) {
		ssize_t n;
		int     err = 0;
		{
			std::shared_lock<std::shared_mutex> fl(_fd_lock);
			if (_fd < 0)
				return;
			n   = ::recv(_fd, _hello + _hello_got, sizeof(_hello) - _hello_got, MSG_DONTWAIT);
			err = errno;
		}
		if (n > 0) {
			_hello_got += size_t(n);
			if (_hello_got < sizeof(_hello))
				continue;
			_handshaking = false;
			update_mask(0, EPOLLIN); // data waits in the kernel until reads are armed
			datapath::detail::hello h{};
			if (datapath::detail::decode_hello(_hello, h) != datapath::error::Success) {
				// Answer with our own hello so the client can report
				// VersionMismatch, then close once it is written.
				_close_when_flushed = true;
				complete_accept();
				return;
			}
			uint64_t limit = std::min(_own_limit, h.max_message_size);
			_parser.set_limit(limit);
			_queue.set_limit(limit);
			if (auto l = _listener.lock())
				l->offer(self());
			else
				close_with(datapath::error::Closed);
			return;
		}
		if (n < 0 && err == EINTR)
			continue;
		if (n < 0 && (err == EAGAIN || err == EWOULDBLOCK))
			return;
		close_with(n == 0 ? datapath::error::Closed : map_errno(err));
		return;
	}
}

void datapath::linux_::uds_connection::complete_accept()
{
	_wbatch.resize(datapath::detail::hello_size);
	datapath::detail::encode_hello(_wbatch.data(), {datapath::detail::protocol_version, _own_limit});
	_woff = 0;
	write_loop();
}

void datapath::linux_::uds_connection::platform_close()
{
	{
		std::unique_lock<std::shared_mutex> fl(_fd_lock);
		if (_fd >= 0) {
			_loop.remove_fd(_fd);
			::shutdown(_fd, SHUT_RDWR);
			::close(_fd);
			_fd = -1;
		}
	}
	// The fd no longer produces events: always notify the loop explicitly.
	if (!_loop.post(self(), k_closed))
		finish_close();
}

void datapath::linux_::uds_connection::platform_arm()
{
	if (!_loop.post(self(), k_arm)) {
		close_with(datapath::error::Closed);
		finish_close();
	}
}

void datapath::linux_::uds_connection::platform_post_close()
{
	// platform_close() already posted k_closed.
}

void datapath::linux_::uds_connection::platform_write()
{
	write_loop();
}

void datapath::linux_::uds_connection::after_inline_write()
{
	if (_ctx->yield_enabled())
		sched_yield();
}

void datapath::linux_::uds_connection::on_finished()
{
	if (auto l = _listener.lock())
		l->connection_finished();
}

void datapath::linux_::uds_connection::on_event(uint32_t events)
{
	if (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
		if (_write_parked.exchange(false, std::memory_order_acq_rel)) {
			update_mask(0, EPOLLOUT);
			write_loop();
		}
	}
	if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
		if (_handshaking)
			hello_read();
		else if (armed())
			read_loop();
	}
}

void datapath::linux_::uds_connection::on_post(int kind)
{
	switch (kind) {
	case k_arm:
		update_mask(EPOLLIN, 0);
		read_loop();
		break;
	case k_closed:
		finish_close();
		_loop.unregister(this);
		break;
	}
}
