/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "uds_server.hpp"
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>
#include "platform.hpp"
#include "uds_connection.hpp"

namespace {
	// True if the socket file at `addr` has no server behind it. Linux refuses
	// connections only when nobody listens (a full backlog is EAGAIN); macOS
	// also refuses them while a live server's backlog is full, so it asks a few
	// times before declaring the file stale.
	bool stale_socket(const datapath::posix::socket_address& addr)
	{
#ifdef __APPLE__
		constexpr int attempts = 20;
#else
		constexpr int attempts = 1;
#endif
		for (int i = 0; i < attempts; i++) {
			if (i)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
			if (probe < 0)
				return false;
			int rc  = ::connect(probe, reinterpret_cast<const sockaddr*>(&addr.addr), addr.length);
			int err = errno;
			::close(probe);
			if (rc == 0 || err != ECONNREFUSED)
				return false;
		}
		return true;
	}
} // namespace

datapath::posix::uds_listener::uds_listener(std::shared_ptr<event_context> ctx, event_loop& loop,
											 socket_address addr, const datapath::options& opts)
	: _ctx(std::move(ctx)), _loop(loop), _addr(std::move(addr)), _opts(opts)
{}

datapath::posix::uds_listener::~uds_listener()
{
	if (_fd >= 0)
		::close(_fd);
}

datapath::error datapath::posix::uds_listener::create()
{
	_fd = open_stream_socket();
	if (_fd < 0)
		return datapath::error::CriticalFailure;

	auto do_bind = [&] { return ::bind(_fd, reinterpret_cast<const sockaddr*>(&_addr.addr), _addr.length); };
	if (do_bind() != 0) {
		if (errno != EADDRINUSE)
			return errno == EACCES ? datapath::error::AccessDenied : datapath::error::InvalidPath;
		if (_addr.abstract)
			return datapath::error::AddressInUse;
		// A stale socket file is reused only if it is a socket nobody answers on.
		struct stat st {};
		if (::lstat(_addr.filesystem_path.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode))
			return datapath::error::AddressInUse;
		if (!stale_socket(_addr))
			return datapath::error::AddressInUse;
		::unlink(_addr.filesystem_path.c_str());
		if (do_bind() != 0)
			return datapath::error::AddressInUse;
	}
	if (!_addr.abstract) {
		_unlink_on_close = true;
		mode_t mode      = 0600;
		if (_opts.permissions == datapath::permissions::Group)
			mode = 0660;
		else if (_opts.permissions == datapath::permissions::World)
			mode = 0666;
		// Before listen(): nobody can connect until the mode is right.
		if (::chmod(_addr.filesystem_path.c_str(), mode) != 0)
			return datapath::error::AccessDenied;
		// BSDs give a new file the group of its directory (wheel for /tmp),
		// not the creator's: Group access means the creator's group.
		if (_opts.permissions == datapath::permissions::Group
			&& ::chown(_addr.filesystem_path.c_str(), uid_t(-1), ::getegid()) != 0)
			return datapath::error::AccessDenied;
	}
	if (::listen(_fd, int(std::max<size_t>(_opts.backlog, 16))) != 0)
		return datapath::error::Failure;
	if (!_loop.add(_fd, ev::in, shared_from_this()))
		return datapath::error::CriticalFailure;
	return datapath::error::Success;
}

bool datapath::posix::uds_listener::peer_allowed(int fd) const
{
	if (!_addr.abstract || _opts.permissions == datapath::permissions::World)
		return true; // filesystem sockets are guarded by their mode bits
	uid_t uid = 0;
	gid_t gid = 0;
	if (!peer_credentials(fd, uid, gid))
		return false;
	if (uid == 0 || uid == ::geteuid())
		return true;
	return _opts.permissions == datapath::permissions::Group && gid == ::getegid();
}

void datapath::posix::uds_listener::on_event(uint32_t)
{
	for (;;) {
		int fd;
		{
			std::lock_guard<std::mutex> l(_lock);
			if (_closed || _fd < 0)
				return;
			fd = accept_stream_socket(_fd);
		}
		if (fd < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			return; // EAGAIN, or out of descriptors: wait for the next edge
		}
		std::shared_ptr<uds_connection> conn;
		{
			std::lock_guard<std::mutex> l(_lock);
			bool full = _opts.max_clients != 0 && _connected >= _opts.max_clients;
			if (_closed || full || !peer_allowed(fd)) {
				::close(fd);
				continue;
			}
			_connected++;
			tune_socket_buffers(fd, _opts);
			conn = std::make_shared<uds_connection>(_ctx, _ctx->pick_loop(), fd, _opts, _opts.max_message_size);
			_conns.erase(std::remove_if(_conns.begin(), _conns.end(),
										[](const decltype(_conns)::value_type& w) { return w.expired(); }),
						 _conns.end());
			_conns.push_back(conn);
		}
		_ctx->add_member(std::weak_ptr<datapath::detail::connection>(conn));
		if (!conn->start(true, weak_from_this(), _opts.max_message_size))
			conn->close();
	}
}

void datapath::posix::uds_listener::on_post(int kind)
{
	if (kind == 0) {
		_loop.unregister(this);
		return;
	}
	if (kind != k_offer)
		return;
	for (;;) {
		std::shared_ptr<uds_connection> c;
		{
			std::lock_guard<std::mutex> l(_lock);
			if (_waiting.empty() || !_handler) {
				_offer_posted = false;
				return;
			}
			c = std::move(_waiting.front());
			_waiting.pop_front();
		}
		offer(std::move(c));
	}
}

void datapath::posix::uds_listener::offer(std::shared_ptr<uds_connection> conn)
{
	datapath::detail::accept_fn handler;
	{
		std::lock_guard<std::mutex> l(_lock);
		if (_closed) {
			conn->close();
			return;
		}
		if (!_handler) {
			_waiting.push_back(std::move(conn));
			return;
		}
		handler = _handler;
	}
	std::lock_guard<std::recursive_mutex> cb(_callback_lock);
	{
		std::lock_guard<std::mutex> l(_lock);
		if (_closed) {
			conn->close();
			return;
		}
	}
	bool accepted = false;
	try {
		accepted = handler(conn);
	} catch (...) {
		accepted = false;
	}
	if (accepted && conn->good())
		conn->complete_accept();
	else
		conn->close();
}

void datapath::posix::uds_listener::connection_finished()
{
	std::lock_guard<std::mutex> l(_lock);
	if (_connected)
		_connected--;
}

void datapath::posix::uds_listener::set_accept_handler(datapath::detail::accept_fn handler)
{
	std::lock_guard<std::mutex> l(_lock);
	_handler = std::move(handler);
	if (_handler && !_waiting.empty() && !_offer_posted)
		_offer_posted = _loop.post(shared_from_this(), k_offer);
}

datapath::error datapath::posix::uds_listener::close()
{
	std::vector<std::weak_ptr<uds_connection>>  conns;
	std::deque<std::shared_ptr<uds_connection>> waiting;
	{
		std::lock_guard<std::mutex> l(_lock);
		if (_closed)
			return datapath::error::Closed;
		_closed = true;
		if (_fd >= 0) {
			_loop.remove_fd(_fd);
			::close(_fd);
			_fd = -1;
		}
		if (_unlink_on_close)
			::unlink(_addr.filesystem_path.c_str());
		conns.swap(_conns);
		waiting.swap(_waiting);
		_handler = nullptr;
	}
	for (auto& c : waiting)
		c->close();
	for (auto& w : conns)
		if (auto c = w.lock())
			c->close();
	// Drop the loop's reference on the loop thread.
	_loop.post(shared_from_this(), 0);
	return datapath::error::Success;
}

void datapath::posix::uds_listener::release()
{
	close();
	std::lock_guard<std::recursive_mutex> cb(_callback_lock);
}
