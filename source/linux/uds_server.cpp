/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "uds_server.hpp"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include "uds_connection.hpp"

datapath::linux_::uds_listener::uds_listener(std::shared_ptr<epoll_context> ctx, event_loop& loop,
											 socket_address addr, const datapath::options& opts)
	: _ctx(std::move(ctx)), _loop(loop), _addr(std::move(addr)), _opts(opts)
{}

datapath::linux_::uds_listener::~uds_listener()
{
	if (_fd >= 0)
		::close(_fd);
}

datapath::error datapath::linux_::uds_listener::create()
{
	_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (_fd < 0)
		return datapath::error::CriticalFailure;

	auto do_bind = [&] { return ::bind(_fd, reinterpret_cast<const sockaddr*>(&_addr.addr), _addr.length); };
	if (do_bind() != 0) {
		if (errno != EADDRINUSE)
			return errno == EACCES ? datapath::error::AccessDenied : datapath::error::InvalidPath;
		if (_addr.abstract)
			return datapath::error::AddressInUse;
		// A stale socket file is reused only if nobody answers on it.
		int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		int rc    = ::connect(probe, reinterpret_cast<const sockaddr*>(&_addr.addr), _addr.length);
		int err   = errno;
		::close(probe);
		if (rc == 0 || err != ECONNREFUSED)
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
	}
	if (::listen(_fd, int(std::max<size_t>(_opts.backlog, 16))) != 0)
		return datapath::error::Failure;
	if (!_loop.add(_fd, EPOLLIN, shared_from_this()))
		return datapath::error::CriticalFailure;
	return datapath::error::Success;
}

bool datapath::linux_::uds_listener::peer_allowed(int fd) const
{
	if (!_addr.abstract || _opts.permissions == datapath::permissions::World)
		return true; // filesystem sockets are guarded by their mode bits
	ucred     cred{};
	socklen_t len = sizeof(cred);
	if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
		return false;
	if (cred.uid == 0 || cred.uid == ::geteuid())
		return true;
	return _opts.permissions == datapath::permissions::Group && cred.gid == ::getegid();
}

void datapath::linux_::uds_listener::on_event(uint32_t)
{
	for (;;) {
		int fd;
		{
			std::lock_guard<std::mutex> l(_lock);
			if (_closed || _fd < 0)
				return;
			fd = ::accept4(_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
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
			conn = std::make_shared<uds_connection>(_ctx, _ctx->pick_loop(), fd, _opts, _opts.max_message_size);
			_conns.erase(std::remove_if(_conns.begin(), _conns.end(), [](auto& w) { return w.expired(); }),
						 _conns.end());
			_conns.push_back(conn);
		}
		_ctx->add_member(std::weak_ptr<datapath::detail::connection>(conn));
		if (!conn->start(true, weak_from_this(), _opts.max_message_size))
			conn->close();
	}
}

void datapath::linux_::uds_listener::on_post(int kind)
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

void datapath::linux_::uds_listener::offer(std::shared_ptr<uds_connection> conn)
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

void datapath::linux_::uds_listener::connection_finished()
{
	std::lock_guard<std::mutex> l(_lock);
	if (_connected)
		_connected--;
}

void datapath::linux_::uds_listener::set_accept_handler(datapath::detail::accept_fn handler)
{
	std::lock_guard<std::mutex> l(_lock);
	_handler = std::move(handler);
	if (_handler && !_waiting.empty() && !_offer_posted)
		_offer_posted = _loop.post(shared_from_this(), k_offer);
}

datapath::error datapath::linux_::uds_listener::close()
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

void datapath::linux_::uds_listener::release()
{
	close();
	std::lock_guard<std::recursive_mutex> cb(_callback_lock);
}
