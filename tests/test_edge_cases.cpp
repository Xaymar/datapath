/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Edge cases and less common paths of the public API. Platform-neutral.

#include <datapath/datapath.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "check.hpp"
#include "common/detail.hpp"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using datapath::error;
using sock_ptr = std::shared_ptr<datapath::socket>;
using srv_ptr  = std::shared_ptr<datapath::server>;

static std::string unique_name()
{
	static std::atomic<int> counter{0};
	return "dp-edge-" + std::to_string(getpid()) + "-" + std::to_string(counter++);
}

struct latch {
	std::mutex              m;
	std::condition_variable cv;
	int                     count = 0;

	void notify()
	{
		std::lock_guard<std::mutex> l(m);
		count++;
		cv.notify_all();
	}
	bool wait(int n, std::chrono::milliseconds timeout = 10s)
	{
		std::unique_lock<std::mutex> l(m);
		return cv.wait_for(l, timeout, [&] { return count >= n; });
	}
	int value()
	{
		std::lock_guard<std::mutex> l(m);
		return count;
	}
};

// Connect on another thread so the caller can act on the server meanwhile.
static std::future<std::pair<error, sock_ptr>> connect_async(const std::string& name, datapath::options o = {},
															 std::shared_ptr<datapath::context> ctx = nullptr)
{
	return std::async(std::launch::async, [name, o, ctx] {
		sock_ptr s;
		error    ec = datapath::connect(s, name, o, ctx);
		return std::make_pair(ec, s);
	});
}

TEST(error_to_string_out_of_range)
{
	CHECK(std::strcmp(datapath::to_string(static_cast<error>(0x7FFF)), "Invalid") == 0);
}

TEST(empty_path_is_invalid)
{
	srv_ptr  srv;
	sock_ptr s;
	CHECK_EQ(datapath::host(srv, ""), error::InvalidPath);
	CHECK_EQ(datapath::connect(s, ""), error::InvalidPath);
	CHECK(!srv);
	CHECK(!s);
}

// A client that connects before on_accept is set waits for the handler.
TEST(client_waits_for_late_accept_handler)
{
	auto    name = unique_name();
	srv_ptr server;
	CHECK_EQ(datapath::host(server, name), error::Success);
	datapath::options o;
	o.connect_timeout = 5000ms;
	auto pending      = connect_async(name, o);
	std::this_thread::sleep_for(100ms);
	sock_ptr keep;
	latch    accepted;
	server->on_accept([&](sock_ptr s) {
		keep = s;
		accepted.notify();
		return true;
	});
	auto r = pending.get();
	CHECK_EQ(r.first, error::Success);
	CHECK(accepted.wait(1));
	CHECK(r.second->good());
}

// handshake_timeout ends once the hello is in: waiting for on_accept does not count.
TEST(late_accept_handler_outlasts_handshake_timeout)
{
	auto              name = unique_name();
	srv_ptr           server;
	datapath::options so;
	so.handshake_timeout = 50ms;
	CHECK_EQ(datapath::host(server, name, so), error::Success);
	datapath::options o;
	o.connect_timeout = 5000ms;
	auto pending      = connect_async(name, o);
	std::this_thread::sleep_for(300ms);
	sock_ptr keep;
	latch    accepted;
	server->on_accept([&](sock_ptr s) {
		keep = s;
		accepted.notify();
		return true;
	});
	auto r = pending.get();
	CHECK_EQ(r.first, error::Success);
	CHECK(accepted.wait(1));
	CHECK(r.second->good());
	CHECK(keep->good());
}

// Clearing the handler makes new clients wait again until a new one is set.
TEST(cleared_accept_handler_holds_clients)
{
	auto    name = unique_name();
	srv_ptr server;
	CHECK_EQ(datapath::host(server, name), error::Success);
	std::vector<sock_ptr> keep;
	std::mutex            lock;
	latch                 accepted;
	auto                  handler = [&](sock_ptr s) {
        std::lock_guard<std::mutex> l(lock);
        keep.push_back(s);
        accepted.notify();
        return true;
	};
	server->on_accept(handler);
	sock_ptr first;
	CHECK_EQ(datapath::connect(first, name), error::Success);
	CHECK(accepted.wait(1));

	server->on_accept(nullptr);
	datapath::options o;
	o.connect_timeout = 5000ms;
	auto pending      = connect_async(name, o);
	std::this_thread::sleep_for(100ms);
	CHECK_EQ(accepted.value(), 1);
	server->on_accept(handler);
	auto r = pending.get();
	CHECK_EQ(r.first, error::Success);
	CHECK(accepted.wait(2));
}

TEST(throwing_accept_handler_rejects)
{
	auto    name = unique_name();
	srv_ptr server;
	latch   asked;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr) -> bool {
		asked.notify();
		throw std::runtime_error("handler failure");
	});
	sock_ptr c;
	CHECK(datapath::connect(c, name) != error::Success);
	CHECK_EQ(asked.value(), 1);
	// The server survives and keeps asking.
	CHECK(datapath::connect(c, name) != error::Success);
	CHECK_EQ(asked.value(), 2);
}

TEST(max_clients_limits_concurrent_connections)
{
	// Declared first so they outlive the handles whose callbacks use them.
	latch                 closed;
	std::mutex            lock;
	std::vector<sock_ptr> keep;
	auto                  name = unique_name();
	srv_ptr               server;
	datapath::options     so;
	so.max_clients = 1;
	CHECK_EQ(datapath::host(server, name, so), error::Success);
	server->on_accept([&](sock_ptr s) {
		// Reads (and so close detection) start once a message handler is set.
		s->on_message([](const uint8_t*, size_t) {});
		s->on_close([&](error) { closed.notify(); });
		std::lock_guard<std::mutex> l(lock);
		keep.push_back(s);
		return true;
	});
	datapath::options co;
	co.connect_timeout = 500ms;
	sock_ptr a, b;
	CHECK_EQ(datapath::connect(a, name, co), error::Success);
	CHECK(datapath::connect(b, name, co) != error::Success);

	// Once the first client is gone, a slot frees up.
	CHECK_EQ(a->close(), error::Success);
	CHECK(closed.wait(1));
	error ec = error::Unknown;
	for (int i = 0; i < 50 && ec != error::Success; i++) {
		ec = datapath::connect(b, name, co);
		if (ec != error::Success)
			std::this_thread::sleep_for(20ms);
	}
	CHECK_EQ(ec, error::Success);
}

TEST(server_close_is_idempotent)
{
	auto    name = unique_name();
	srv_ptr server;
	CHECK_EQ(datapath::host(server, name), error::Success);
	CHECK_EQ(server->close(), error::Success);
	CHECK_EQ(server->close(), error::Closed);
	// Setting a handler on a closed server is harmless.
	server->on_accept([](sock_ptr) { return true; });
	sock_ptr c;
	CHECK(datapath::connect(c, name) != error::Success);
}

// Clients still waiting for an accept handler are dropped when the server closes.
TEST(server_close_drops_waiting_clients)
{
	auto    name = unique_name();
	srv_ptr server;
	CHECK_EQ(datapath::host(server, name), error::Success);
	datapath::options o;
	o.connect_timeout = 5000ms;
	auto pending      = connect_async(name, o);
	std::this_thread::sleep_for(100ms);
	auto start = std::chrono::steady_clock::now();
	CHECK_EQ(server->close(), error::Success);
	auto r = pending.get();
	CHECK(r.first != error::Success);
	CHECK(std::chrono::steady_clock::now() - start < 4000ms);
}

TEST(connect_times_out_without_accept_handler)
{
	auto    name = unique_name();
	srv_ptr server;
	CHECK_EQ(datapath::host(server, name), error::Success);
	datapath::options o;
	o.connect_timeout = 100ms;
	sock_ptr c;
	auto     start = std::chrono::steady_clock::now();
	CHECK_EQ(datapath::connect(c, name, o), error::TimedOut);
	CHECK(std::chrono::steady_clock::now() - start < 5000ms);
	CHECK(!c);
}

TEST(stopped_context_refuses_work)
{
	auto ctx = datapath::context::create();
	CHECK(ctx != nullptr);
	ctx->impl()->shutdown();
	srv_ptr  srv;
	sock_ptr s;
	CHECK_EQ(datapath::host(srv, unique_name(), {}, ctx), error::Closed);
	CHECK_EQ(datapath::connect(s, unique_name(), {}, ctx), error::Closed);
	ctx->impl()->shutdown(); // twice is harmless
}

TEST(socket_calls_after_close)
{
	auto     name = unique_name();
	srv_ptr  server;
	sock_ptr keep;
	latch    peer_closed;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr s) {
		s->on_message([](const uint8_t*, size_t) {});
		s->on_close([&](error) { peer_closed.notify(); });
		keep = s;
		return true;
	});
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	CHECK_EQ(c->close(), error::Success);
	CHECK(!c->good());
	CHECK_EQ(c->close(), error::Closed);
	CHECK_EQ(c->send("x", 1), error::Closed);
	datapath::buffer_view part{"y", 1};
	CHECK_EQ(c->sendv(&part, 1), error::Closed);
	CHECK(peer_closed.wait(1));
	CHECK(!keep->good());
	CHECK_EQ(keep->send("z", 1), error::Closed);

	// A close handler set after the close never fires late.
	std::atomic<int> late{0};
	c->on_close([&](error) { late++; });
	c->on_message([](const uint8_t*, size_t) {});
	std::this_thread::sleep_for(50ms);
	CHECK(late.load() <= 1);
}

// Gather sends with zero parts and with empty parts produce empty messages.
TEST(sendv_empty_parts)
{
	auto                name = unique_name();
	srv_ptr             server;
	sock_ptr            keep;
	std::mutex          lock;
	std::vector<size_t> sizes;
	latch               got;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr s) {
		s->on_message([&](const uint8_t*, size_t n) {
			{
				std::lock_guard<std::mutex> l(lock);
				sizes.push_back(n);
			}
			got.notify();
		});
		keep = s;
		return true;
	});
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	CHECK_EQ(c->sendv(nullptr, 0), error::Success);
	datapath::buffer_view parts[] = {{nullptr, 0}, {"ab", 2}, {nullptr, 0}, {"c", 1}};
	CHECK_EQ(c->sendv(parts, 4), error::Success);
	CHECK(got.wait(2));
	std::lock_guard<std::mutex> l(lock);
	CHECK_EQ(sizes[0], size_t(0));
	CHECK_EQ(sizes[1], size_t(3));
}

// Every context option combination must still carry traffic.
TEST(context_option_variants_echo)
{
	struct variant {
		int  spin_us;
		bool yield;
		int  priority;
		int  ideal_core;
		bool alertable;
		bool mmcss;
	} variants[] = {
		{0, false, 0, -1, false, false}, {0, true, 1, 0, false, false},   {10, true, 2, -1, true, false},
		{100, false, 3, 0, false, true}, {50, false, -1, -1, true, true}, {50, false, 99, 1, false, false},
	};
	for (auto& v : variants) {
		datapath::context_options co;
		co.spin       = std::chrono::microseconds(v.spin_us);
		co.yield      = v.yield;
		co.priority   = v.priority;
		co.ideal_core = v.ideal_core;
		co.alertable  = v.alertable;
		co.mmcss      = v.mmcss;
		co.io_threads = 2;
		auto ctx      = datapath::context::create(co);
		CHECK(ctx != nullptr);

		auto     name = unique_name();
		srv_ptr  server;
		sock_ptr keep;
		CHECK_EQ(datapath::host(server, name, {}, ctx), error::Success);
		server->on_accept([&](sock_ptr s) {
			std::weak_ptr<datapath::socket> weak = s;
			s->on_message([weak](const uint8_t* d, size_t n) {
				if (auto p = weak.lock())
					p->send(d, n);
			});
			keep = s;
			return true;
		});
		sock_ptr c;
		latch    got;
		CHECK_EQ(datapath::connect(c, name, {}, ctx), error::Success);
		c->on_message([&](const uint8_t*, size_t) { got.notify(); });
		for (int i = 0; i < 20; i++)
			CHECK_EQ(c->send(&i, sizeof(i)), error::Success);
		CHECK(got.wait(20));
		c.reset();
		keep.reset();
		server.reset();
	}
}

// Closing a socket while other threads keep sending must never crash or hang.
TEST(close_while_sending)
{
	auto     name = unique_name();
	srv_ptr  server;
	sock_ptr keep;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr s) {
		s->on_message([](const uint8_t*, size_t) {});
		keep = s;
		return true;
	});
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	std::atomic<bool>        stop{false};
	std::atomic<int>         closed_seen{0};
	std::vector<std::thread> ts;
	for (int t = 0; t < 4; t++)
		ts.emplace_back([&] {
			std::vector<uint8_t> p(4096);
			while (!stop) {
				error ec = c->send(p.data(), p.size());
				if (ec == error::Closed) {
					closed_seen++;
					return;
				}
			}
		});
	std::this_thread::sleep_for(50ms);
	c->close();
	stop = true;
	for (auto& t : ts)
		t.join();
	CHECK(!c->good());
}

// The server side closing makes the client see Closed with its close handler.
TEST(server_side_close_reaches_client)
{
	auto     name = unique_name();
	srv_ptr  server;
	sock_ptr keep;
	latch    accepted;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr s) {
		keep = s;
		accepted.notify();
		return true;
	});
	sock_ptr c;
	latch    closed;
	error    reason = error::Unknown;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	c->on_close([&](error e) {
		reason = e;
		closed.notify();
	});
	c->on_message([](const uint8_t*, size_t) {});
	CHECK(accepted.wait(1));
	CHECK_EQ(keep->close(), error::Success);
	CHECK(closed.wait(1));
	CHECK_EQ(reason, error::Closed);
}

TEST_MAIN()
