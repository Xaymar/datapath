/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "event_context.hpp"
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>
#include "common/handshake.hpp"
#include "common/spin.hpp"
#include "path.hpp"
#include "platform.hpp"
#include "shm.hpp"
#include "uds_connection.hpp"
#include "uds_server.hpp"

namespace {
	// The context whose I/O thread is running on this thread, if any.
	thread_local const datapath::posix::event_context* tl_context = nullptr;

	// A connected stream socket to `addr`, retrying a full backlog until
	// `deadline`.
	datapath::error open_connected(const datapath::posix::socket_address& addr,
								   std::chrono::steady_clock::time_point deadline, const datapath::options& opts,
								   int& out)
	{
		using namespace datapath::posix;
		using clock = std::chrono::steady_clock;
		int fd      = open_stream_socket();
		if (fd < 0)
			return datapath::error::CriticalFailure;
		tune_socket_buffers(fd, opts);
		for (;;) {
			if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr.addr), addr.length) == 0)
				break;
			int err = errno;
			if (err == EINTR)
				continue;
#ifdef __APPLE__
			// A full backlog refuses the connection instead of failing with EAGAIN,
			// the same way a socket file without a server does. Keep trying while
			// the file exists; the deadline reports TimedOut like a full backlog.
			if (err == ECONNREFUSED) {
				struct stat st {};
				if (::stat(addr.filesystem_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode))
					err = EAGAIN;
			}
#endif
			if (err == EAGAIN && clock::now() < deadline) {
				// Backlog full: the server is busy or stalled. Retry until the deadline.
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifdef __APPLE__
				// BSD sockets are unusable after a failed connect(): start over.
				::close(fd);
				fd = open_stream_socket();
				if (fd < 0)
					return datapath::error::CriticalFailure;
				tune_socket_buffers(fd, opts);
#endif
				continue;
			}
			::close(fd);
			switch (err) {
			case ENOENT:
			case ECONNREFUSED:
				return datapath::error::NotFound;
			case EACCES:
			case EPERM:
				return datapath::error::AccessDenied;
			case EAGAIN:
				return datapath::error::TimedOut;
			default:
				return datapath::error::Failure;
			}
		}
		out = fd;
		return datapath::error::Success;
	}
} // namespace

// event_loop -------------------------------------------------------------------

datapath::posix::event_loop::event_loop(event_context* ctx) : _ctx(ctx) {}

datapath::posix::event_loop::~event_loop()
{
	close_poller();
}

bool datapath::posix::event_loop::start()
{
	if (!open_poller())
		return false;
	// The thread keeps its context alive, so the context is never destroyed on
	// one of its own I/O threads.
	auto keep = _ctx->shared_from_this();
	_thread   = std::thread([this, keep] {
        tl_context = keep.get();
		apply_thread_priority(keep->priority());
		run();
    });
	return true;
}

void datapath::posix::event_loop::unregister(fd_target* target)
{
	std::shared_ptr<fd_target> dying;
	{
		std::lock_guard<std::mutex> l(_reg_lock);
		auto                        it = _registered.find(target);
		if (it == _registered.end())
			return;
		dying = std::move(it->second);
		_registered.erase(it);
	}
	// `dying` may be the last reference; it is released outside the lock.
}

bool datapath::posix::event_loop::post(std::shared_ptr<fd_target> target, int kind)
{
	bool signal;
	{
		std::lock_guard<std::mutex> l(_post_lock);
		if (_dead)
			return false;
		_posts.push_back({std::move(target), kind});
		signal    = !_signaled;
		_signaled = true;
	}
	if (signal)
		wake();
	return true;
}

// Called by pump() once the wake-up has been consumed.
bool datapath::posix::event_loop::drain_posts(bool& quit)
{
	std::vector<post_item> items;
	{
		std::lock_guard<std::mutex> l(_post_lock);
		items.swap(_posts);
		_signaled = false;
	}
	for (auto& it : items) {
		if (!it.target) {
			quit = true;
			continue;
		}
		it.target->on_post(it.kind);
	}
	return !items.empty();
}

void datapath::posix::event_loop::add_poller(std::shared_ptr<poll_target> t)
{
	std::lock_guard<std::mutex> l(_poll_lock);
	_pollers.push_back(std::move(t));
	_poll_version.fetch_add(1, std::memory_order_release);
}

void datapath::posix::event_loop::remove_poller(poll_target* t)
{
	std::shared_ptr<poll_target> dying;
	{
		std::lock_guard<std::mutex> l(_poll_lock);
		for (auto it = _pollers.begin(); it != _pollers.end(); ++it)
			if (it->get() == t) {
				dying = std::move(*it);
				_pollers.erase(it);
				break;
			}
		_poll_version.fetch_add(1, std::memory_order_release);
	}
}

void datapath::posix::event_loop::run()
{
	bool quit = false;
	// Shared-memory rings this loop busy-polls along with its descriptors. The
	// list is copied whenever it changes (never while idle).
	std::vector<std::shared_ptr<poll_target>> polled;
	uint64_t                                  polled_version = ~uint64_t(0);
	auto                                      refresh        = [&] {
        uint64_t v = _poll_version.load(std::memory_order_acquire);
        if (v == polled_version)
            return;
        std::lock_guard<std::mutex> l(_poll_lock);
        polled         = _pollers;
        polled_version = _poll_version.load(std::memory_order_relaxed);
	};
	auto poll = [&] {
		bool work = pump(0, quit);
		refresh();
		for (auto& t : polled)
			work |= t->poll();
		return work;
	};
	while (!quit) {
		auto block = [&] {
			if (quit)
				return;
			refresh();
			bool work = false;
			for (auto& t : polled)
				if (!t->prepare_block())
					work = true;
			if (work) {
				// Work arrived meanwhile: handle it here (with no spin budget this is
				// the only place rings get polled), then spin again.
				poll();
				return;
			}
			pump(-1, quit);
		};
		if (_ctx->yield_enabled())
			datapath::detail::spin_then_block(_ctx->spin(), [&] { return !quit && poll(); }, block,
											  [] { sched_yield(); });
		else
			datapath::detail::spin_then_block(_ctx->spin(), [&] { return !quit && poll(); }, block);
	}
	polled.clear();
}

// event_context ----------------------------------------------------------------

datapath::posix::event_context::event_context(const datapath::context_options& opts) : _opts(opts)
{
	if (_opts.io_threads == 0) {
		// Automatic: one I/O thread per physical core.
		_opts.io_threads = physical_core_count();
		if (_opts.io_threads == 0)
			_opts.io_threads = std::thread::hardware_concurrency();
	}
	if (_opts.io_threads == 0)
		_opts.io_threads = 1;
}

datapath::posix::event_context::~event_context()
{
	shutdown();
}

bool datapath::posix::event_context::start()
{
	for (size_t i = 0; i < _opts.io_threads; i++) {
		_loops.push_back(datapath::detail::make_unique<event_loop>(this));
		if (!_loops.back()->start())
			return false;
	}
	return true;
}

datapath::posix::event_loop& datapath::posix::event_context::pick_loop()
{
	return *_loops[_next.fetch_add(1, std::memory_order_relaxed) % _loops.size()];
}

void datapath::posix::event_context::add_member(std::weak_ptr<datapath::detail::connection> c)
{
	std::lock_guard<std::mutex> l(_members_lock);
	if (_connections.size() >= 64 && (_connections.size() & (_connections.size() - 1)) == 0)
		_connections.erase(std::remove_if(_connections.begin(), _connections.end(),
										  [](const decltype(_connections)::value_type& w) { return w.expired(); }),
						   _connections.end());
	_connections.push_back(std::move(c));
}

void datapath::posix::event_context::add_member(std::weak_ptr<datapath::detail::listener> l)
{
	std::lock_guard<std::mutex> g(_members_lock);
	_listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(),
									[](const decltype(_listeners)::value_type& w) { return w.expired(); }),
					 _listeners.end());
	_listeners.push_back(std::move(l));
}

bool datapath::posix::event_context::on_io_thread() const noexcept
{
	return tl_context == this;
}

void datapath::posix::event_context::shutdown()
{
	if (_stopped.exchange(true))
		return;
	if (on_io_thread()) {
		// An I/O thread cannot join itself, and its caller's stack still uses
		// objects the teardown would free: finish on a separate thread.
		auto self = shared_from_this();
		std::thread([self] { self->shutdown_now(); }).detach();
		return;
	}
	shutdown_now();
}

void datapath::posix::event_context::shutdown_now()
{
	// No deadline may close a connection behind the teardown's back.
	_watchdog.stop();
	std::vector<std::weak_ptr<datapath::detail::connection>> conns;
	std::vector<std::weak_ptr<datapath::detail::listener>>   lsts;
	{
		std::lock_guard<std::mutex> l(_members_lock);
		conns.swap(_connections);
		lsts.swap(_listeners);
	}
	for (auto& w : lsts)
		if (auto l = w.lock())
			l->close();
	for (auto& w : conns)
		if (auto c = w.lock())
			c->close();
	// Close notifications were posted above; the quit item queues behind them.
	for (auto& loop : _loops)
		loop->post(nullptr, 0);
	for (auto& loop : _loops)
		if (loop->_thread.joinable())
			loop->_thread.join();
	for (auto& loop : _loops) {
		// Notifications that raced the quit item still run (e.g. on_close).
		std::vector<event_loop::post_item> posts;
		{
			std::lock_guard<std::mutex> l(loop->_post_lock);
			loop->_dead = true;
			posts.swap(loop->_posts);
		}
		for (auto& it : posts)
			if (it.target)
				it.target->on_post(it.kind);
		// Break context <-> target reference cycles.
		std::unordered_map<fd_target*, std::shared_ptr<fd_target>> regs;
		std::vector<std::shared_ptr<poll_target>>                  pollers;
		{
			std::lock_guard<std::mutex> l(loop->_reg_lock);
			regs.swap(loop->_registered);
		}
		{
			std::lock_guard<std::mutex> l(loop->_poll_lock);
			pollers.swap(loop->_pollers);
		}
	}
}

datapath::error datapath::posix::event_context::connect(std::shared_ptr<datapath::detail::connection>& out,
														 const std::string& path, const datapath::options& opts)
{
	if (stopped())
		return datapath::error::Closed;
	socket_address addr;
	if (!make_socket_address(path, addr))
		return datapath::error::InvalidPath;

	using clock   = std::chrono::steady_clock;
	auto deadline = clock::now() + opts.connect_timeout;

	// Version 3 when asking for shared memory, else version 1. A server that
	// doesn't know version 3 answers with version 1 and closes: connect again.
	bool want_ext = shm_link::wanted(opts);
	for (;;) {
		int fd = -1;
		auto ec = open_connected(addr, deadline, opts, fd);
		if (ec != datapath::error::Success)
			return ec;

		int  fds[shm_fd_count] = {-1, -1, -1, -1, -1};
		auto drop_fds          = [&] {
            for (int& f : fds)
                if (f >= 0)
                    ::close(f), f = -1;
		};
		// Reads and writes with the connect deadline. Reads also collect the
		// descriptors the server sends with its version 3 reply.
		auto transfer = [&](bool write, uint8_t* buf, size_t size) -> datapath::error {
			size_t done = 0;
			while (done < size) {
				int    left = int(std::max<int64_t>(
                    0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count()));
				pollfd pfd{fd, short(write ? POLLOUT : POLLIN), 0};
				int    pr = ::poll(&pfd, 1, left);
				if (pr < 0 && errno == EINTR)
					continue;
				if (pr == 0)
					return datapath::error::TimedOut;
				ssize_t n;
				if (write) {
					n = ::send(fd, buf + done, size - done, send_flags);
				} else {
					alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int) * shm_fd_count)] = {};
					iovec                 iov{buf + done, size - done};
					msghdr                msg{};
					msg.msg_iov        = &iov;
					msg.msg_iovlen     = 1;
					msg.msg_control    = ctl;
					msg.msg_controllen = sizeof(ctl);
					int flags          = MSG_DONTWAIT;
#ifdef MSG_CMSG_CLOEXEC
					flags |= MSG_CMSG_CLOEXEC;
#endif
					n = ::recvmsg(fd, &msg, flags);
					for (cmsghdr* cm = n >= 0 ? CMSG_FIRSTHDR(&msg) : nullptr; cm; cm = CMSG_NXTHDR(&msg, cm)) {
						if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
							continue;
						size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
						for (size_t i = 0; i < count; i++) {
							int got;
							std::memcpy(&got, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
							bool kept = false;
							for (int& f : fds)
								if (f < 0 && !kept)
									f = got, kept = true;
							if (!kept)
								::close(got); // more than a link has: dropped
						}
					}
					if (msg.msg_flags & MSG_CTRUNC) {
						drop_fds();
						return datapath::error::VersionMismatch;
					}
				}
				if (n > 0) {
					done += size_t(n);
					continue;
				}
				if (n < 0 && (errno == EINTR || errno == EAGAIN))
					continue;
				return datapath::error::Closed;
			}
			return datapath::error::Success;
		};

		uint8_t hello[datapath::detail::hello_size + datapath::detail::ext_trailer_size];
		datapath::detail::encode_hello(hello, {want_ext ? datapath::detail::protocol_version_ext
														: datapath::detail::protocol_version,
											   opts.max_message_size});
		size_t hello_out = datapath::detail::hello_size;
		if (want_ext) {
			datapath::detail::encode_ext_trailer(hello + datapath::detail::hello_size,
												 datapath::detail::ext_shared_memory, 0);
			hello_out += datapath::detail::ext_trailer_size;
		}
		ec = transfer(true, hello, hello_out);
		if (ec == datapath::error::Success)
			ec = transfer(false, hello, datapath::detail::hello_size);
		datapath::detail::hello reply{};
		if (ec == datapath::error::Success)
			ec = want_ext ? datapath::detail::decode_hello_any(hello, reply) : datapath::detail::decode_hello(hello, reply);
		bool    fallback = false, got_shm = false;
		uint8_t offer[shm_offer_size] = {};
		if (ec == datapath::error::Success && want_ext) {
			if (reply.version == datapath::detail::protocol_version_ext) {
				uint8_t trailer[datapath::detail::ext_trailer_size];
				ec = transfer(false, trailer, sizeof(trailer));
				uint32_t cpu = 0, accepted = 0;
				if (ec == datapath::error::Success) {
					datapath::detail::decode_ext_trailer(trailer, cpu, accepted);
					got_shm = (accepted & datapath::detail::ext_shared_memory) != 0;
				}
				if (ec == datapath::error::Success && got_shm)
					ec = transfer(false, offer, sizeof(offer));
			} else if (reply.version == datapath::detail::protocol_version) {
				fallback = true;
			} else {
				ec = datapath::error::VersionMismatch;
			}
		}
		if (ec != datapath::error::Success || fallback) {
			drop_fds();
			::close(fd);
			if (fallback) {
				want_ext = false;
				continue;
			}
			return ec;
		}

		auto                      limit = std::min(opts.max_message_size, reply.max_message_size);
		std::unique_ptr<shm_link> shm;
		if (got_shm) {
			ec = shm_link::open(shm, fd, offer, fds, limit, opts.max_queued_bytes);
			if (ec != datapath::error::Success) {
				::close(fd);
				return ec;
			}
		}
		drop_fds(); // sent without being offered
		auto conn = std::make_shared<uds_connection>(shared_from_this(), pick_loop(), fd, opts, limit);
		if (shm)
			conn->use_shared_memory(std::move(shm));
		if (!conn->start(false, {}, limit))
			return datapath::error::CriticalFailure;
		add_member(std::weak_ptr<datapath::detail::connection>(conn));
		out = std::move(conn);
		return datapath::error::Success;
	}
}

datapath::error datapath::posix::event_context::listen(std::shared_ptr<datapath::detail::listener>& out,
														const std::string& path, const datapath::options& opts)
{
	if (stopped())
		return datapath::error::Closed;
	socket_address addr;
	if (!make_socket_address(path, addr))
		return datapath::error::InvalidPath;
	auto lst = datapath::detail::make_shared_self<uds_listener>(shared_from_this(), pick_loop(), std::move(addr), opts);
	auto ec  = lst->create();
	if (ec != datapath::error::Success) {
		lst->close();
		return ec;
	}
	add_member(std::weak_ptr<datapath::detail::listener>(lst));
	out = std::move(lst);
	return datapath::error::Success;
}

std::shared_ptr<datapath::detail::context_impl> datapath::detail::make_platform_context(
	const datapath::context_options& options)
{
	auto ctx = std::make_shared<datapath::posix::event_context>(options);
	if (!ctx->start())
		return nullptr;
	return ctx;
}
