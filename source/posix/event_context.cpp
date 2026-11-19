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
#include <thread>
#include "common/handshake.hpp"
#include "common/spin.hpp"
#include "path.hpp"
#include "platform.hpp"
#include "uds_connection.hpp"
#include "uds_server.hpp"

namespace {
	// The context whose I/O thread is running on this thread, if any.
	thread_local const datapath::posix::event_context* tl_context = nullptr;
}

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
	_thread = std::thread([this, keep = _ctx->shared_from_this()] {
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

void datapath::posix::event_loop::run()
{
	bool quit = false;
	while (!quit) {
		auto block = [&] {
			if (!quit)
				pump(-1, quit);
		};
		if (_ctx->yield_enabled())
			datapath::detail::spin_then_block(_ctx->spin(), [&] { return !quit && pump(0, quit); }, block,
											  [] { sched_yield(); });
		else
			datapath::detail::spin_then_block(_ctx->spin(), [&] { return !quit && pump(0, quit); }, block);
	}
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
		_loops.push_back(std::make_unique<event_loop>(this));
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
		_connections.erase(std::remove_if(_connections.begin(), _connections.end(), [](auto& w) { return w.expired(); }),
						   _connections.end());
	_connections.push_back(std::move(c));
}

void datapath::posix::event_context::add_member(std::weak_ptr<datapath::detail::listener> l)
{
	std::lock_guard<std::mutex> g(_members_lock);
	_listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(), [](auto& w) { return w.expired(); }),
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
		std::thread([self = shared_from_this()] { self->shutdown_now(); }).detach();
		return;
	}
	shutdown_now();
}

void datapath::posix::event_context::shutdown_now()
{
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
		{
			std::lock_guard<std::mutex> l(loop->_reg_lock);
			regs.swap(loop->_registered);
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

	int fd = open_stream_socket();
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
			ssize_t n = write ? ::send(fd, buf + done, size - done, send_flags)
							  : ::recv(fd, buf + done, size - done, MSG_DONTWAIT);
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

	uint8_t hello[datapath::detail::hello_size];
	datapath::detail::encode_hello(hello, {datapath::detail::protocol_version, opts.max_message_size});
	datapath::error ec = transfer(true, hello, sizeof(hello));
	if (ec == datapath::error::Success)
		ec = transfer(false, hello, sizeof(hello));
	datapath::detail::hello reply{};
	if (ec == datapath::error::Success)
		ec = datapath::detail::decode_hello(hello, reply);
	if (ec != datapath::error::Success) {
		::close(fd);
		return ec;
	}
	auto limit = std::min(opts.max_message_size, reply.max_message_size);
	auto conn  = std::make_shared<uds_connection>(shared_from_this(), pick_loop(), fd, opts, limit);
	if (!conn->start(false, {}, limit))
		return datapath::error::CriticalFailure;
	add_member(std::weak_ptr<datapath::detail::connection>(conn));
	out = std::move(conn);
	return datapath::error::Success;
}

datapath::error datapath::posix::event_context::listen(std::shared_ptr<datapath::detail::listener>& out,
														const std::string& path, const datapath::options& opts)
{
	if (stopped())
		return datapath::error::Closed;
	socket_address addr;
	if (!make_socket_address(path, addr))
		return datapath::error::InvalidPath;
	auto lst = std::make_shared<uds_listener>(shared_from_this(), pick_loop(), std::move(addr), opts);
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
