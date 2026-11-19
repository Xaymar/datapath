/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "epoll_context.hpp"
#include <poll.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include "common/handshake.hpp"
#include "common/spin.hpp"
#include "path.hpp"
#include "uds_connection.hpp"
#include "uds_server.hpp"

namespace {
	// The context whose I/O thread is running on this thread, if any.
	thread_local const datapath::linux_::epoll_context* tl_context = nullptr;

	// Raise the calling thread's priority by lowering its nice value, relative to
	// the nice value it inherited (like Windows thread priorities are relative to
	// the process's priority class). The default level 2 needs a large gap: the
	// scheduler shares CPU by weight, and against a full load at the same nice
	// value 64 B p99.9 was ~8 ms at a gap of 0, ~4 ms at 8, ~200 us at 12 and
	// ~160 us at 19. Level 3 matches Android's THREAD_PRIORITY_URGENT_AUDIO.
	// Unprivileged processes may only go down to 20 - RLIMIT_NICE; the thread then
	// gets the lowest nice value it is allowed, or stays where it was.
	// Real-time policies (SCHED_FIFO/RR) are not used: a spinning real-time thread
	// can starve the rest of the system.
	void apply_priority(int level)
	{
		static const int steps[] = {0, -4, -12, -19};
		if (level <= 0 || level > 3)
			return;
		pid_t tid = ::gettid(); // Linux: PRIO_PROCESS with a thread id affects only that thread
		errno     = 0;
		int base  = ::getpriority(PRIO_PROCESS, id_t(tid));
		if (base == -1 && errno != 0)
			return;
		int want = std::max(-20, base + steps[level]);
		if (::setpriority(PRIO_PROCESS, id_t(tid), want) == 0)
			return;
		rlimit rl{};
		if (::getrlimit(RLIMIT_NICE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY)
			return;
		int floor = 20 - int(std::min<rlim_t>(rl.rlim_cur, 40));
		if (floor > want && floor < base)
			::setpriority(PRIO_PROCESS, id_t(tid), floor);
	}
}

// event_loop -------------------------------------------------------------------

datapath::linux_::event_loop::event_loop(epoll_context* ctx) : _ctx(ctx) {}

datapath::linux_::event_loop::~event_loop()
{
	if (_efd >= 0)
		::close(_efd);
	if (_ep >= 0)
		::close(_ep);
}

bool datapath::linux_::event_loop::start()
{
	_ep  = ::epoll_create1(EPOLL_CLOEXEC);
	_efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (_ep < 0 || _efd < 0)
		return false;
	epoll_event ev{};
	ev.events   = EPOLLIN;
	ev.data.ptr = nullptr;
	if (::epoll_ctl(_ep, EPOLL_CTL_ADD, _efd, &ev) != 0)
		return false;
	// The thread keeps its context alive, so the context is never destroyed on
	// one of its own I/O threads.
	_thread = std::thread([this, keep = _ctx->shared_from_this()] {
		tl_context = keep.get();
		apply_priority(keep->priority());
		run();
	});
	return true;
}

bool datapath::linux_::event_loop::add(int fd, uint32_t events, std::shared_ptr<fd_target> target)
{
	{
		std::lock_guard<std::mutex> l(_reg_lock);
		_registered[target.get()] = target;
	}
	epoll_event ev{};
	ev.events   = events | EPOLLET;
	ev.data.ptr = target.get();
	if (::epoll_ctl(_ep, EPOLL_CTL_ADD, fd, &ev) != 0) {
		std::lock_guard<std::mutex> l(_reg_lock);
		_registered.erase(target.get());
		return false;
	}
	return true;
}

bool datapath::linux_::event_loop::modify(int fd, uint32_t events, fd_target* target)
{
	epoll_event ev{};
	ev.events   = events | EPOLLET;
	ev.data.ptr = target;
	return ::epoll_ctl(_ep, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void datapath::linux_::event_loop::remove_fd(int fd)
{
	::epoll_ctl(_ep, EPOLL_CTL_DEL, fd, nullptr);
}

void datapath::linux_::event_loop::unregister(fd_target* target)
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

bool datapath::linux_::event_loop::post(std::shared_ptr<fd_target> target, int kind)
{
	bool wake;
	{
		std::lock_guard<std::mutex> l(_post_lock);
		if (_dead)
			return false;
		_posts.push_back({std::move(target), kind});
		wake      = !_signaled;
		_signaled = true;
	}
	if (wake) {
		uint64_t one = 1;
		ssize_t  r   = ::write(_efd, &one, sizeof(one));
		(void)r;
	}
	return true;
}

bool datapath::linux_::event_loop::drain_posts(bool& quit)
{
	uint64_t v;
	ssize_t  r = ::read(_efd, &v, sizeof(v));
	(void)r;
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

void datapath::linux_::event_loop::run()
{
	epoll_event events[64];
	bool        quit = false;
	auto        pump = [&](int timeout) {
        int n = ::epoll_wait(_ep, events, 64, timeout);
        if (n <= 0)
            return false;
        bool posts = false;
        for (int i = 0; i < n; i++) {
            auto* t = static_cast<fd_target*>(events[i].data.ptr);
            if (!t) {
                posts = true;
                continue;
            }
            t->on_event(events[i].events);
        }
        if (posts)
            drain_posts(quit);
        return true;
	};
	while (!quit) {
		auto block = [&] {
			if (!quit)
				pump(-1);
		};
		if (_ctx->yield_enabled())
			datapath::detail::spin_then_block(_ctx->spin(), [&] { return !quit && pump(0); }, block,
											  [] { sched_yield(); });
		else
			datapath::detail::spin_then_block(_ctx->spin(), [&] { return !quit && pump(0); }, block);
	}
}

// epoll_context ----------------------------------------------------------------

namespace {
	// Physical cores this process may run on: CPUs in the affinity mask that are
	// the first hyper-thread of their core. 0 if the topology is unavailable.
	size_t physical_core_count()
	{
		cpu_set_t set;
		CPU_ZERO(&set);
		if (sched_getaffinity(0, sizeof(set), &set) != 0)
			return 0;
		size_t n = 0;
		for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
			if (!CPU_ISSET(cpu, &set))
				continue;
			char path[96];
			std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
			FILE* f = std::fopen(path, "r");
			if (!f)
				return 0;
			int  sibling = -1;
			bool ok      = std::fscanf(f, "%d", &sibling) == 1;
			std::fclose(f);
			if (!ok)
				return 0;
			// The first sibling counts for the core, unless it is outside the mask.
			if (sibling == cpu || !CPU_ISSET(sibling, &set))
				n++;
		}
		return n;
	}
} // namespace

datapath::linux_::epoll_context::epoll_context(const datapath::context_options& opts) : _opts(opts)
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

datapath::linux_::epoll_context::~epoll_context()
{
	shutdown();
}

bool datapath::linux_::epoll_context::start()
{
	for (size_t i = 0; i < _opts.io_threads; i++) {
		_loops.push_back(std::make_unique<event_loop>(this));
		if (!_loops.back()->start())
			return false;
	}
	return true;
}

datapath::linux_::event_loop& datapath::linux_::epoll_context::pick_loop()
{
	return *_loops[_next.fetch_add(1, std::memory_order_relaxed) % _loops.size()];
}

void datapath::linux_::epoll_context::add_member(std::weak_ptr<datapath::detail::connection> c)
{
	std::lock_guard<std::mutex> l(_members_lock);
	if (_connections.size() >= 64 && (_connections.size() & (_connections.size() - 1)) == 0)
		_connections.erase(std::remove_if(_connections.begin(), _connections.end(), [](auto& w) { return w.expired(); }),
						   _connections.end());
	_connections.push_back(std::move(c));
}

void datapath::linux_::epoll_context::add_member(std::weak_ptr<datapath::detail::listener> l)
{
	std::lock_guard<std::mutex> g(_members_lock);
	_listeners.erase(std::remove_if(_listeners.begin(), _listeners.end(), [](auto& w) { return w.expired(); }),
					 _listeners.end());
	_listeners.push_back(std::move(l));
}

bool datapath::linux_::epoll_context::on_io_thread() const noexcept
{
	return tl_context == this;
}

void datapath::linux_::epoll_context::shutdown()
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

void datapath::linux_::epoll_context::shutdown_now()
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

datapath::error datapath::linux_::epoll_context::connect(std::shared_ptr<datapath::detail::connection>& out,
														 const std::string& path, const datapath::options& opts)
{
	if (stopped())
		return datapath::error::Closed;
	socket_address addr;
	if (!make_socket_address(path, addr))
		return datapath::error::InvalidPath;

	using clock   = std::chrono::steady_clock;
	auto deadline = clock::now() + opts.connect_timeout;

	int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return datapath::error::CriticalFailure;
	for (;;) {
		if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr.addr), addr.length) == 0)
			break;
		int err = errno;
		if (err == EINTR)
			continue;
		if (err == EAGAIN && clock::now() < deadline) {
			// Backlog full: the server is busy or stalled. Retry until the deadline.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
			ssize_t n = write ? ::send(fd, buf + done, size - done, MSG_NOSIGNAL | MSG_DONTWAIT)
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

datapath::error datapath::linux_::epoll_context::listen(std::shared_ptr<datapath::detail::listener>& out,
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
	auto ctx = std::make_shared<datapath::linux_::epoll_context>(options);
	if (!ctx->start())
		return nullptr;
	return ctx;
}
