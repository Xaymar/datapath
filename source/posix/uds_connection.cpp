/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "uds_connection.hpp"
#include <sys/socket.h>
#include <sys/uio.h>
#include <sched.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include "common/endian.hpp"
#include "common/framing.hpp"
#include "platform.hpp"
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

datapath::posix::uds_connection::uds_connection(std::shared_ptr<event_context> ctx, event_loop& loop, int fd,
												 const datapath::options& opts, uint64_t negotiated_limit)
	: connection_base(opts, negotiated_limit), _ctx(std::move(ctx)), _loop(loop), _fd(fd),
	  _mask(ev::rdhup), _shm_allowed(shm_link::wanted(opts)),
	  _shm_zero_copy(opts.shared_memory_zero_copy), _shm_size(opts.shared_memory_size),
	  _max_queued(opts.max_queued_bytes)
{}

// An eventfd of the shared-memory link, registered with the loop.
// It holds the connection weakly: the connection owns it.
struct datapath::posix::uds_connection::waker : fd_target {
	std::weak_ptr<uds_connection> conn;
	bool                          space;
	int                           fd;
	waker(std::shared_ptr<uds_connection> c, bool s, int f) : conn(std::move(c)), space(s), fd(f) {}
	void on_event(uint32_t) override
	{
		if (auto c = conn.lock())
			c->on_wake(space, fd);
	}
	void on_post(int) override {}
};

datapath::posix::uds_connection::~uds_connection()
{
	if (_fd >= 0)
		::close(_fd);
}

bool datapath::posix::uds_connection::start(bool handshake, std::weak_ptr<uds_listener> listener,
											 uint64_t own_limit)
{
	if (_shm && !add_waker(true)) // client: the reply is in, messages go through shared memory now
		return false;
	if (handshake) {
		_handshaking = true;
		_listener    = std::move(listener);
		_own_limit   = own_limit;
		_queue.try_acquire(); // the reply goes out before anything the user queues
		_mask |= ev::in;
	}
	return _loop.add(_fd, _mask, self());
}

void datapath::posix::uds_connection::update_mask(uint32_t add, uint32_t remove)
{
	datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
	if (_fd < 0)
		return;
	std::lock_guard<std::mutex> l(_mask_lock);
	uint32_t                    mask = (_mask | add) & ~remove;
	if (mask == _mask)
		return;
	_loop.modify(_fd, _mask, mask, this);
	_mask = mask;
}

void datapath::posix::uds_connection::read_loop()
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
			datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
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

void datapath::posix::uds_connection::write_loop()
{
	for (;;) {
		if (_woff >= _wbatch.size()) {
			_woff = 0;
			if (_shm && !_shm_ready.load(std::memory_order_acquire)) {
				// The server's reply is out: messages go through shared memory now.
				activate_shm();
				return;
			}
			if (!_queue.take(_wbatch)) {
				if (_close_when_flushed)
					close_with(datapath::error::VersionMismatch);
				return;
			}
		}
		ssize_t n;
		int     err = 0;
		{
			datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
			if (_fd < 0)
				return;
			n   = ::send(_fd, _wbatch.data() + _woff, _wbatch.size() - _woff, send_flags);
			err = errno;
		}
		if (n > 0) {
			_woff += size_t(n);
			continue;
		}
		if (n < 0 && err == EINTR)
			continue;
		if (n < 0 && (err == EAGAIN || err == EWOULDBLOCK)) {
			// Hand the writer slot to the loop: it resumes on ev::out. Enabling
			// ev::out re-checks readiness, so no wake-up is lost.
			_write_parked.store(true, std::memory_order_release);
			update_mask(ev::out, 0);
			return;
		}
		close_with(map_errno(err));
		return;
	}
}

void datapath::posix::uds_connection::hello_read()
{
	while (_handshaking && !_closed.load(std::memory_order_acquire)) {
		ssize_t n;
		int     err = 0;
		{
			datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
			if (_fd < 0)
				return;
			n   = ::recv(_fd, _hello + _hello_got, _hello_need - _hello_got, MSG_DONTWAIT);
			err = errno;
		}
		if (n > 0) {
			_hello_got += size_t(n);
			if (_hello_got < _hello_need)
				continue;
			datapath::detail::hello h{};
			// Versions 1 and 3; version 2 (core pairing) is Windows-only and gets
			// the version 1 answer, after which the client connects again.
			bool known = datapath::detail::decode_hello_any(_hello, h) == datapath::error::Success
						 && h.version != datapath::detail::protocol_version_pair;
			if (known && h.version == datapath::detail::protocol_version_ext
				&& _hello_need == datapath::detail::hello_size) {
				_hello_need = sizeof(_hello); // the request trailer follows
				continue;
			}
			_handshaking = false;
			if (!handshake_finished())
				return; // too late: handshake_timeout is closing the connection
			update_mask(0, ev::in); // data waits in the kernel until reads are armed
			if (!known) {
				// Answer with our own hello so the client can report
				// VersionMismatch, then close once it is written.
				_close_when_flushed = true;
				complete_accept();
				return;
			}
			uint64_t limit = std::min(_own_limit, h.max_message_size);
			_parser.set_limit(limit);
			_queue.set_limit(limit);
			if (h.version == datapath::detail::protocol_version_ext) {
				_ext          = true;
				uint32_t want = 0, reserved = 0;
				datapath::detail::decode_ext_trailer(_hello + datapath::detail::hello_size, want, reserved);
				if ((want & datapath::detail::ext_shared_memory) && _shm_allowed) {
					// Decided before the connection is offered, so that its read
					// chain already knows which path to read. Without it, or if the
					// objects can't be made, the connection stays on the socket.
					datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
					if (_fd >= 0
						&& shm_link::create(_shm, _fd, _shm_size, limit, _max_queued, _shm_zero_copy, _offer, _offer_fds)
							   == datapath::error::Success)
						_ext_accepted |= datapath::detail::ext_shared_memory;
					else
						_shm.reset();
				}
			}
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

void datapath::posix::uds_connection::complete_accept()
{
	if (_ext) {
		// Version 3: no pairing on this platform (-1), the granted extensions,
		// then the offer. The link's descriptors travel with the reply.
		size_t n = datapath::detail::hello_size + datapath::detail::ext_trailer_size;
		_wbatch.resize(n + (_shm ? shm_offer_size : 0));
		datapath::detail::encode_hello(_wbatch.data(), {datapath::detail::protocol_version_ext, _own_limit});
		datapath::detail::encode_ext_trailer(_wbatch.data() + datapath::detail::hello_size, uint32_t(-1),
											 _ext_accepted);
		_woff = 0;
		if (_shm) {
			std::memcpy(_wbatch.data() + n, _offer, shm_offer_size);
			if (!send_reply_with_fds())
				return;
		}
	} else {
		_wbatch.resize(datapath::detail::hello_size);
		datapath::detail::encode_hello(_wbatch.data(), {datapath::detail::protocol_version, _own_limit});
		_woff = 0;
	}
	write_loop();
}

bool datapath::posix::uds_connection::send_reply_with_fds()
{
	// The first bytes of the reply carry the descriptors; write_loop() sends
	// whatever the socket did not take.
	alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int) * shm_fd_count)] = {};
	iovec  iov{_wbatch.data(), _wbatch.size()};
	msghdr msg{};
	msg.msg_iov        = &iov;
	msg.msg_iovlen     = 1;
	msg.msg_control    = ctl;
	msg.msg_controllen = sizeof(ctl);
	cmsghdr* cm        = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level     = SOL_SOCKET;
	cm->cmsg_type      = SCM_RIGHTS;
	cm->cmsg_len       = CMSG_LEN(sizeof(int) * shm_fd_count);
	std::memcpy(CMSG_DATA(cm), _offer_fds, sizeof(int) * shm_fd_count);
	for (;;) {
		ssize_t r;
		int     err = 0;
		{
			datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
			if (_fd < 0)
				return false;
			r   = ::sendmsg(_fd, &msg, send_flags);
			err = errno;
		}
		if (r > 0) {
			_woff = size_t(r);
			return true;
		}
		if (r < 0 && err == EINTR)
			continue;
		// A fresh socket always takes the ~80 bytes; anything else is fatal.
		close_with(r < 0 ? map_errno(err) : datapath::error::Failure);
		return false;
	}
}

void datapath::posix::uds_connection::platform_close()
{
	{
		std::unique_lock<datapath::detail::shared_mutex> fl(_fd_lock);
		if (_data_waker)
			_loop.remove_fd(_data_waker->fd);
		if (_space_waker)
			_loop.remove_fd(_space_waker->fd);
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

void datapath::posix::uds_connection::platform_arm()
{
	if (!_loop.post(self(), k_arm)) {
		close_with(datapath::error::Closed);
		finish_close();
	}
}

void datapath::posix::uds_connection::platform_post_close()
{
	// platform_close() already posted k_closed.
}

void datapath::posix::uds_connection::platform_write()
{
	if (_shm_ready.load(std::memory_order_acquire)) {
		// Queued by a send() that ran before activation.
		flush_queue_to_shm();
		return;
	}
	write_loop();
}

bool datapath::posix::uds_connection::direct_write_wanted(uint64_t frame_size) const noexcept
{
	return frame_size > datapath::detail::send_queue::coalesce_limit;
}

size_t datapath::posix::uds_connection::platform_write_direct(const uint8_t* header, size_t header_size,
															 const buffer_view* parts, size_t count)
{
	// One sendmsg() for header and payload: no copy, and the peer never sees
	// the header without the data that follows it.
	constexpr size_t     max_iov = 64;
	struct iovec         iov[max_iov];
	size_t               n       = 0;
	iov[n++]                     = {const_cast<uint8_t*>(header), header_size};
	for (size_t i = 0; i < count && n < max_iov; i++)
		if (parts[i].size)
			iov[n++] = {const_cast<void*>(parts[i].data), parts[i].size};
	// More parts than iovecs: send what fits, the rest is queued by the caller.
	size_t sent = 0;
	for (;;) {
		msghdr msg{};
		msg.msg_iov    = iov;
		msg.msg_iovlen = decltype(msg.msg_iovlen)(n);
		ssize_t r;
		int     err = 0;
		{
			datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
			if (_fd < 0)
				return sent;
			r   = ::sendmsg(_fd, &msg, send_flags);
			err = errno;
		}
		if (r >= 0)
			return sent + size_t(r);
		if (err == EINTR)
			continue;
		if (err == EAGAIN || err == EWOULDBLOCK)
			return sent;
		close_with(map_errno(err));
		return 0;
	}
}

void datapath::posix::uds_connection::after_inline_write()
{
	if (_ctx->yield_enabled())
		sched_yield();
}

void datapath::posix::uds_connection::on_finished()
{
	if (auto l = _listener.lock())
		l->connection_finished();
}

void datapath::posix::uds_connection::on_event(uint32_t events)
{
	if (events & (ev::out | ev::err | ev::hup)) {
		if (_write_parked.exchange(false, std::memory_order_acq_rel)) {
			update_mask(0, ev::out);
			write_loop();
		}
	}
	if (events & (ev::in | ev::rdhup | ev::hup | ev::err)) {
		if (_shm_reading)
			shm_peer_gone(); // the socket only ever reports the peer's end
		else if (_shm)
			; // start_shm_reading() checks for a hang-up that came before it
		else if (_handshaking)
			hello_read();
		else if (armed())
			read_loop();
	}
}

void datapath::posix::uds_connection::on_post(int kind)
{
	switch (kind) {
	case k_arm:
		if (_shm) {
			start_shm_reading();
			break;
		}
		update_mask(ev::in, 0);
		read_loop();
		break;
	case k_closed:
		if (_shm_reading)
			_loop.remove_poller(this);
		finish_close();
		{
			datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
			if (_data_waker)
				_loop.unregister(_data_waker.get());
			if (_space_waker)
				_loop.unregister(_space_waker.get());
		}
		_loop.unregister(this);
		break;
	}
}

// ---------------------------------------------------------------- shared memory

void datapath::posix::uds_connection::use_shared_memory(std::unique_ptr<shm_link> shm)
{
	_shm = std::move(shm);
	_shm_ready.store(true, std::memory_order_release);
	_shm_active.store(true, std::memory_order_release);
}

datapath::error datapath::posix::uds_connection::send(const buffer_view* parts, size_t count)
{
	if (!_shm_active.load(std::memory_order_acquire))
		return connection_base::send(parts, count);
	if (_closed.load(std::memory_order_acquire))
		return datapath::error::Closed;
	bool need = false;
	auto ec   = _shm->send(parts, count, need);
	if (ec == datapath::error::Closed)
		close_with(datapath::error::Failure);
	else if (need)
		_shm->prepare_space_wait(); // the space eventfd resumes it
	return ec;
}

void datapath::posix::uds_connection::activate_shm()
{
	// Whoever holds the writer slot from now on writes to the ring. send() goes
	// to the ring directly only once the queue was drained, so that a message a
	// thread queued before activation never lands behind one it sent after.
	if (!add_waker(true)) {
		close_with(datapath::error::CriticalFailure);
		return;
	}
	_shm_ready.store(true, std::memory_order_release);
	flush_queue_to_shm();
	_shm_active.store(true, std::memory_order_release);
}

void datapath::posix::uds_connection::flush_queue_to_shm()
{
	// Writer-slot holder: move what send() queued for the socket into the ring,
	// in order. Blocks hold whole frames (length header + payload). take()
	// releases the writer slot once the queue is empty.
	bool need = false;
	while (_queue.take(_wbatch)) {
		size_t off = 0;
		while (off + datapath::detail::frame_header_size <= _wbatch.size()) {
			uint64_t len = datapath::detail::load_le64(_wbatch.data() + off);
			off += datapath::detail::frame_header_size;
			if (len > _wbatch.size() - off)
				break;
			buffer_view v{_wbatch.data() + off, size_t(len)};
			bool        n = false;
			if (_shm->send(&v, 1, n) == datapath::error::Closed)
				close_with(datapath::error::Failure);
			need |= n;
			off += size_t(len);
		}
	}
	_wbatch.clear();
	_woff = 0;
	if (need)
		_shm->prepare_space_wait();
}

void datapath::posix::uds_connection::start_shm_reading()
{
	if (!add_waker(false)) {
		close_with(datapath::error::CriticalFailure);
		return;
	}
	_shm_reading = true;
	_loop.add_poller(std::static_pointer_cast<poll_target>(self()));
	// The peer never writes to the socket after the handshake. A hang-up that
	// came before reading started produced its edge already: check for it.
	char    probe;
	ssize_t r;
	{
		datapath::detail::shared_lock<datapath::detail::shared_mutex> fl(_fd_lock);
		r = _fd < 0 ? 0 : ::recv(_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
	}
	if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
		shm_peer_gone();
	else
		poll();
}

void datapath::posix::uds_connection::shm_peer_gone()
{
	// Messages the peer put in the ring before it went away are still delivered.
	if (!_closed.load(std::memory_order_acquire)) {
		while (_polling.test_and_set(std::memory_order_acquire))
			sched_yield();
		_shm->poll([this](const uint8_t* d, size_t n) { return deliver_message(d, n); });
		_polling.clear(std::memory_order_release);
	}
	close_with(datapath::error::Closed);
}

bool datapath::posix::uds_connection::add_waker(bool space)
{
	// Under the descriptor lock, so platform_close() either sees it and removes
	// it from the loop, or ran before and nothing is registered.
	auto                                w = std::make_shared<waker>(self(), space,
																	space ? _shm->tx_space_fd() : _shm->rx_data_fd());
	std::unique_lock<datapath::detail::shared_mutex> fl(_fd_lock);
	if (_fd < 0)
		return true; // closed: k_closed follows
	if (!_loop.add(w->fd, ev::in, w))
		return false;
	(space ? _space_waker : _data_waker) = std::move(w);
	return true;
}

void datapath::posix::uds_connection::on_wake(bool space, int fd)
{
	shm_link::clear(fd);
	if (space) {
		bool need = false;
		if (!_shm->drain(need))
			close_with(datapath::error::Failure);
		else if (need)
			_shm->prepare_space_wait();
		return;
	}
	poll();
}

bool datapath::posix::uds_connection::poll()
{
	if (_closed.load(std::memory_order_acquire) || _polling.test_and_set(std::memory_order_acquire))
		return false;
	int r = _shm->poll([this](const uint8_t* d, size_t n) { return deliver_message(d, n); });
	_polling.clear(std::memory_order_release);
	if (r < 0)
		close_with(datapath::error::Failure);
	return r > 0;
}

bool datapath::posix::uds_connection::prepare_block()
{
	if (_closed.load(std::memory_order_acquire))
		return true;
	return _shm->prepare_block();
}
