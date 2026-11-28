/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// End-to-end behaviour of the public API. Platform-neutral.

#include <datapath/datapath.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "check.hpp"
#include "common/framing.hpp"
#include "common/handshake.hpp"
#include "raw_peer.hpp"

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
using bytes    = std::vector<uint8_t>;

static std::string unique_name()
{
	static std::atomic<int> counter{0};
	return "dp-test-" + std::to_string(getpid()) + "-" + std::to_string(counter++);
}

// Counting latch with timeout.
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

// Server that echoes every message back and keeps its clients alive.
struct echo_server {
	// Declared first so they outlive the handles below.
	latch                 accepted;
	latch                 closed;
	std::mutex            lock;
	srv_ptr               server;
	std::vector<sock_ptr> clients;

	error start(const std::string& name, const datapath::options& opts = {})
	{
		error ec = datapath::host(server, name, opts);
		if (ec != error::Success)
			return ec;
		server->on_accept([this](sock_ptr s) {
			std::weak_ptr<datapath::socket> weak = s;
			s->on_close([this](error) { closed.notify(); });
			s->on_message([weak](const uint8_t* d, size_t n) {
				if (auto c = weak.lock())
					c->send(d, n);
			});
			{
				std::lock_guard<std::mutex> l(lock);
				clients.push_back(s);
			}
			accepted.notify();
			return true;
		});
		return ec;
	}
};

// Client collecting every message it receives.
struct collector {
	std::mutex         lock;
	std::vector<bytes> messages;
	latch              received;
	latch              closed;
	std::atomic<int>   close_calls{0};
	error              close_reason = error::Unknown;

	void attach(datapath::socket& s)
	{
		s.on_close([this](error e) {
			close_reason = e;
			close_calls++;
			closed.notify();
		});
		s.on_message([this](const uint8_t* d, size_t n) {
			{
				std::lock_guard<std::mutex> l(lock);
				messages.emplace_back(d, d + n);
			}
			received.notify();
		});
	}
};

static bytes pattern(size_t n, uint32_t seed)
{
	bytes b(n);
	for (size_t i = 0; i < n; i++)
		b[i] = uint8_t((i * 2654435761u + seed) >> 13);
	return b;
}

TEST(connect_missing_is_not_found)
{
	sock_ptr s;
	CHECK_EQ(datapath::connect(s, unique_name()), error::NotFound);
	CHECK(!s);
}

TEST(host_twice_is_address_in_use)
{
	auto    name = unique_name();
	srv_ptr a, b;
	CHECK_EQ(datapath::host(a, name), error::Success);
	CHECK_EQ(datapath::host(b, name), error::AddressInUse);
}

TEST(echo_small)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	collector col;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	CHECK(c->good());
	col.attach(*c);
	CHECK_EQ(c->send("hello", 5), error::Success);
	CHECK(col.received.wait(1));
	CHECK(col.messages[0] == bytes({'h', 'e', 'l', 'l', 'o'}));
}

TEST(zero_length)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	collector col;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	col.attach(*c);
	CHECK_EQ(c->send(nullptr, 0), error::Success);
	CHECK_EQ(c->send("x", 1), error::Success);
	CHECK(col.received.wait(2));
	CHECK(col.messages[0].empty());
	CHECK_EQ(col.messages[1].size(), size_t(1));
}

TEST(large_8mib_intact)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	collector col;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	col.attach(*c);
	auto big = pattern(8 << 20, 7);
	// Gather send: header-ish prefix plus payload.
	datapath::buffer_view parts[] = {{"AB", 2}, {big.data(), big.size()}};
	CHECK_EQ(c->sendv(parts, 2), error::Success);
	CHECK(col.received.wait(1, 30s));
	CHECK_EQ(col.messages[0].size(), big.size() + 2);
	CHECK(std::memcmp(col.messages[0].data(), "AB", 2) == 0);
	CHECK(std::memcmp(col.messages[0].data() + 2, big.data(), big.size()) == 0);
}

TEST(ordered_10000)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	collector col;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	col.attach(*c);
	for (uint32_t i = 0; i < 10000; i++) {
		error ec;
		while ((ec = c->send(&i, sizeof(i))) == error::WouldBlock)
			std::this_thread::yield();
		CHECK_EQ(ec, error::Success);
	}
	CHECK(col.received.wait(10000, 30s));
	for (uint32_t i = 0; i < 10000; i++) {
		uint32_t v;
		std::memcpy(&v, col.messages[i].data(), 4);
		CHECK_EQ(v, i);
	}
}

TEST(concurrent_senders)
{
	constexpr int threads = 8, per = 1000;
	auto          name    = unique_name();
	srv_ptr       server;
	CHECK_EQ(datapath::host(server, name), error::Success);

	std::mutex       lock;
	std::vector<int> next(threads, 0);
	std::atomic<int> bad{0};
	latch            got;
	sock_ptr         keep;
	server->on_accept([&](sock_ptr s) {
		keep = s;
		s->on_message([&](const uint8_t* d, size_t n) {
			int t = d[0], i;
			std::memcpy(&i, d + 1, 4);
			size_t want = 5 + size_t((i * 131 + t * 7) % 3000);
			bool   ok   = t < threads && n == want;
			for (size_t k = 5; ok && k < n; k++)
				ok = d[k] == uint8_t(t + i);
			{
				std::lock_guard<std::mutex> l(lock);
				if (!ok || next[t] != i)
					bad++;
				next[t] = i + 1;
			}
			got.notify();
		});
		return true;
	});

	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; t++) {
		ts.emplace_back([&, t] {
			for (int i = 0; i < per; i++) {
				bytes p(5 + size_t((i * 131 + t * 7) % 3000), uint8_t(t + i));
				p[0] = uint8_t(t);
				std::memcpy(&p[1], &i, 4);
				while (c->send(p.data(), p.size()) == error::WouldBlock)
					std::this_thread::yield();
			}
		});
	}
	for (auto& t : ts)
		t.join();
	CHECK(got.wait(threads * per, 30s));
	CHECK_EQ(bad.load(), 0);
}

TEST(concurrent_large_and_small_senders)
{
	// Large messages may be written straight from the caller's buffer while other
	// threads queue small ones; every message must still arrive whole and in
	// per-thread order.
	constexpr int threads = 6, per = 150;
	auto          name    = unique_name();
	srv_ptr       server;
	CHECK_EQ(datapath::host(server, name), error::Success);

	auto size_of = [](int t, int i) -> size_t {
		return (i % 3 == 0) ? 65536 + size_t((i * 977 + t * 31) % 200000) : 5 + size_t((i * 131 + t * 7) % 3000);
	};
	std::mutex       lock;
	std::vector<int> next(threads, 0);
	std::atomic<int> bad{0};
	latch            got;
	sock_ptr         keep;
	server->on_accept([&](sock_ptr s) {
		keep = s;
		s->on_message([&](const uint8_t* d, size_t n) {
			int t = d[0], i;
			std::memcpy(&i, d + 1, 4);
			bool ok = t < threads && n == size_of(t, i);
			for (size_t k = 5; ok && k < n; k++)
				ok = d[k] == uint8_t(t + i + k);
			{
				std::lock_guard<std::mutex> l(lock);
				if (!ok || next[t] != i)
					bad++;
				next[t] = i + 1;
			}
			got.notify();
		});
		return true;
	});

	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	std::vector<std::thread> ts;
	for (int t = 0; t < threads; t++) {
		ts.emplace_back([&, t] {
			for (int i = 0; i < per; i++) {
				bytes p(size_of(t, i));
				for (size_t k = 5; k < p.size(); k++)
					p[k] = uint8_t(t + i + k);
				p[0] = uint8_t(t);
				std::memcpy(&p[1], &i, 4);
				// Split in two parts to exercise gathered direct writes.
				datapath::buffer_view parts[] = {{p.data(), 5}, {p.data() + 5, p.size() - 5}};
				while (c->sendv(parts, 2) == error::WouldBlock)
					std::this_thread::yield();
				// The caller's buffer is reused right away: nothing may still point into it.
				std::fill(p.begin(), p.end(), uint8_t(0xEE));
			}
		});
	}
	for (auto& t : ts)
		t.join();
	CHECK(got.wait(threads * per, 60s));
	CHECK_EQ(bad.load(), 0);
}

TEST(many_clients)
{
	constexpr int clients = 16, per = 100;
	auto          name    = unique_name();
	echo_server   srv;
	CHECK_EQ(srv.start(name), error::Success);
	std::vector<std::unique_ptr<collector>> cols;
	std::vector<sock_ptr>                   socks(clients);
	for (int i = 0; i < clients; i++) {
		CHECK_EQ(datapath::connect(socks[i], name), error::Success);
		cols.push_back(std::make_unique<collector>());
		cols.back()->attach(*socks[i]);
	}
	for (int k = 0; k < per; k++)
		for (int i = 0; i < clients; i++) {
			uint32_t v = uint32_t(i * 1000 + k);
			CHECK_EQ(socks[i]->send(&v, 4), error::Success);
		}
	for (int i = 0; i < clients; i++) {
		CHECK(cols[i]->received.wait(per, 30s));
		for (int k = 0; k < per; k++) {
			uint32_t v;
			std::memcpy(&v, cols[i]->messages[k].data(), 4);
			CHECK_EQ(v, uint32_t(i * 1000 + k));
		}
	}
	CHECK_EQ(srv.accepted.value(), clients);
}

TEST(too_large_negotiated)
{
	auto             name = unique_name();
	echo_server      srv;
	datapath::options so;
	so.max_message_size = 1024;
	CHECK_EQ(srv.start(name, so), error::Success);
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	CHECK_EQ(c->max_message_size(), uint64_t(1024));
	bytes p(2048);
	CHECK_EQ(c->send(p.data(), p.size()), error::TooLarge);
	CHECK_EQ(c->send(p.data(), 1024), error::Success);
	CHECK(c->good());
}

TEST(would_block_when_peer_stalls)
{
	auto     name = unique_name();
	srv_ptr  server;
	sock_ptr keep;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr s) {
		keep = s; // never sets on_message: never reads
		return true;
	});
	datapath::options co;
	co.max_queued_bytes = 4 << 20;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name, co), error::Success);
	bytes  p(1 << 20);
	error  ec    = error::Success;
	size_t total = 0;
	while (total < (size_t(1) << 30) && (ec = c->send(p.data(), p.size())) == error::Success)
		total += p.size();
	CHECK_EQ(ec, error::WouldBlock);
	CHECK(c->good());
}

// Receive backpressure: a paused receiver reads nothing more, so a flooding
// peer runs into WouldBlock instead of making the receiver buffer without bound.
TEST(pause_backpressures_sender)
{
	auto                  name = unique_name();
	srv_ptr               server;
	sock_ptr              keep;
	latch                 accepted, first;
	std::mutex            m;
	std::vector<uint32_t> seen;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr s) {
		keep                  = s;
		datapath::socket* raw = s.get();
		s->on_message([&, raw](const uint8_t* d, size_t n) {
			uint32_t seq = 0;
			if (n >= 4)
				std::memcpy(&seq, d, 4);
			size_t count;
			{
				std::lock_guard<std::mutex> l(m);
				seen.push_back(seq);
				count = seen.size();
			}
			if (count == 1) {
				raw->pause(); // from the handler: nothing after this message
				first.notify();
			}
		});
		accepted.notify();
		return true;
	});
	datapath::options co;
	co.max_queued_bytes = 1 << 20;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name, co), error::Success);
	latch drained;
	c->on_drain([&] { drained.notify(); });
	CHECK(accepted.wait(1));
	bytes    p(4096);
	error    ec   = error::Success;
	uint32_t sent = 0;
	// Without backpressure this would run until the 1 GiB cap.
	while (size_t(sent) * p.size() < (size_t(1) << 30)) {
		std::memcpy(p.data(), &sent, 4);
		ec = c->send(p.data(), p.size());
		if (ec == error::WouldBlock) {
			// The peer's buffers may still be filling: give them a moment once.
			std::this_thread::sleep_for(20ms);
			ec = c->send(p.data(), p.size());
		}
		if (ec != error::Success)
			break;
		sent++;
	}
	CHECK_EQ(ec, error::WouldBlock);
	CHECK(first.wait(1));
	std::this_thread::sleep_for(100ms);
	{
		std::lock_guard<std::mutex> l(m);
		CHECK_EQ(seen.size(), size_t(1));
	}
	CHECK_EQ(drained.value(), 0);
	keep->resume();
	auto deadline = std::chrono::steady_clock::now() + 10s;
	for (;;) {
		{
			std::lock_guard<std::mutex> l(m);
			if (seen.size() >= sent)
				break;
		}
		CHECK(std::chrono::steady_clock::now() < deadline);
		std::this_thread::sleep_for(1ms);
	}
	{
		std::lock_guard<std::mutex> l(m);
		CHECK_EQ(seen.size(), size_t(sent));
		for (uint32_t i = 0; i < sent; i++)
			CHECK_EQ(seen[i], i);
	}
	// The queue that returned WouldBlock ran empty: on_drain fired once.
	CHECK(drained.wait(1));
	CHECK_EQ(c->send(p.data(), p.size()), error::Success);
}

// Paused before the handler is set, from another thread: nothing is delivered,
// and a remote close waits behind the messages that came before it.
TEST(pause_defers_remote_close)
{
	auto                     name = unique_name();
	srv_ptr                  server;
	sock_ptr                 keep;
	latch                    accepted, closed;
	std::mutex               m;
	std::vector<std::string> events;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr s) {
		keep = s;
		s->pause();
		s->on_close([&](error) {
			{
				std::lock_guard<std::mutex> l(m);
				events.push_back("close");
			}
			closed.notify();
		});
		s->on_message([&](const uint8_t* d, size_t n) {
			std::lock_guard<std::mutex> l(m);
			events.emplace_back(reinterpret_cast<const char*>(d), n);
		});
		accepted.notify();
		return true;
	});
	{
		sock_ptr c;
		CHECK_EQ(datapath::connect(c, name), error::Success);
		CHECK_EQ(c->send("one", 3), error::Success);
		CHECK_EQ(c->send("two", 3), error::Success);
		CHECK_EQ(c->send("three", 5), error::Success);
		CHECK(accepted.wait(1));
		std::this_thread::sleep_for(50ms);
	} // the client goes away
	std::this_thread::sleep_for(100ms);
	{
		std::lock_guard<std::mutex> l(m);
		CHECK(events.empty());
	}
	keep->resume();
	CHECK(closed.wait(1));
	std::lock_guard<std::mutex> l(m);
	CHECK_EQ(events.size(), size_t(4));
	CHECK_EQ(events[0], std::string("one"));
	CHECK_EQ(events[1], std::string("two"));
	CHECK_EQ(events[2], std::string("three"));
	CHECK_EQ(events[3], std::string("close"));
}

// A local close of a paused (stalled) connection still fires on_close.
TEST(close_while_paused)
{
	echo_server srv;
	auto        name = unique_name();
	CHECK_EQ(srv.start(name), error::Success);
	sock_ptr  c;
	collector col;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	col.attach(*c);
	CHECK_EQ(c->send("x", 1), error::Success);
	CHECK(col.received.wait(1));
	c->pause();
	CHECK_EQ(c->send("y", 1), error::Success);
	std::this_thread::sleep_for(50ms);
	CHECK_EQ(col.received.value(), 1);
	CHECK_EQ(c->close(), error::Success);
	CHECK(col.closed.wait(1));
	CHECK_EQ(col.close_calls.load(), 1);
	c->resume(); // harmless after close
	std::this_thread::sleep_for(20ms);
	CHECK_EQ(col.close_calls.load(), 1);
}

// Pause and resume many times while echoing: nothing lost, nothing reordered.
TEST(pause_resume_churn)
{
	echo_server srv;
	auto        name = unique_name();
	CHECK_EQ(srv.start(name), error::Success);
	sock_ptr  c;
	collector col;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	col.attach(*c);
	constexpr uint32_t n = 20000;
	std::atomic<bool>  stop{false};
	std::thread        toggler([&] {
        while (!stop.load()) {
            c->pause();
            std::this_thread::yield();
            c->resume();
        }
    });
	for (uint32_t i = 0; i < n; i++) {
		while (c->send(&i, 4) == error::WouldBlock)
			std::this_thread::yield();
	}
	bool ok = col.received.wait(int(n));
	stop    = true;
	toggler.join();
	c->resume();
	CHECK(ok);
	std::lock_guard<std::mutex> l(col.lock);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t v = 0;
		std::memcpy(&v, col.messages[i].data(), 4);
		CHECK_EQ(v, i);
	}
}

TEST(messages_wait_for_handler)
{
	collector col;
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
	CHECK_EQ(datapath::connect(c, name), error::Success);
	CHECK_EQ(c->send("abc", 3), error::Success);
	CHECK_EQ(c->send("de", 2), error::Success);
	CHECK(accepted.wait(1));
	std::this_thread::sleep_for(50ms);
	col.attach(*keep);
	CHECK(col.received.wait(2));
	CHECK_EQ(col.messages[0].size(), size_t(3));
	CHECK_EQ(col.messages[1].size(), size_t(2));
}

TEST(rejected_accept_connect_fails)
{
	auto    name = unique_name();
	srv_ptr server;
	latch   asked;
	CHECK_EQ(datapath::host(server, name), error::Success);
	server->on_accept([&](sock_ptr) {
		asked.notify();
		return false;
	});
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Closed);
	CHECK_EQ(asked.value(), 1);
	// The server keeps serving.
	CHECK_EQ(datapath::connect(c, name), error::Closed);
	CHECK_EQ(asked.value(), 2);
}

TEST(drop_handle_closes_peer)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	collector col;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	col.attach(*c);
	CHECK(srv.accepted.wait(1));
	{
		std::lock_guard<std::mutex> l(srv.lock);
		srv.clients.clear(); // drop the server's only reference
	}
	CHECK(col.closed.wait(1));
	CHECK_EQ(col.close_reason, error::Closed);
	CHECK(!c->good());
	CHECK_EQ(c->send("x", 1), error::Closed);
}

TEST(close_inside_callback)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	std::atomic<int> msgs{0}, closes{0};
	latch            done;
	datapath::socket* raw = c.get();
	c->on_close([&](error) {
		closes++;
		done.notify();
	});
	c->on_message([&, raw](const uint8_t*, size_t) {
		msgs++;
		raw->close();
	});
	for (int i = 0; i < 10; i++)
		c->send("m", 1);
	CHECK(done.wait(1));
	std::this_thread::sleep_for(100ms);
	CHECK_EQ(msgs.load(), 1);
	CHECK_EQ(closes.load(), 1);
}

TEST(drop_inside_callback)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto     c = std::make_shared<sock_ptr>();
	CHECK_EQ(datapath::connect(*c, name), error::Success);
	std::atomic<int> closes{0};
	(*c)->on_close([&](error) { closes++; });
	latch got;
	(*c)->on_message([c, &got](const uint8_t*, size_t) {
		c->reset(); // destroy the last handle from inside its own callback
		got.notify();
	});
	// The server sends, so no other thread is inside a call on *c when the
	// callback destroys it.
	CHECK(srv.accepted.wait(1));
	{
		std::lock_guard<std::mutex> l(srv.lock);
		srv.clients[0]->send("m", 1);
	}
	CHECK(got.wait(1));
	std::this_thread::sleep_for(100ms);
	CHECK_EQ(closes.load(), 0); // dropped handles never call back
	CHECK(!*c);
	CHECK(srv.closed.wait(1)); // the peer saw the close
}

TEST(on_close_exactly_once)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	collector col;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	col.attach(*c);
	CHECK(srv.accepted.wait(1));
	// Close from both ends at once, repeatedly.
	std::thread t([&] { srv.server->close(); });
	// Either side may win the race; the first local close() reports Success
	// only if the peer's close has not been observed yet.
	error first  = c->close();
	error second = c->close();
	t.join();
	CHECK(first == error::Success || first == error::Closed);
	CHECK_EQ(second, error::Closed);
	CHECK(col.closed.wait(1));
	std::this_thread::sleep_for(100ms);
	CHECK_EQ(col.close_calls.load(), 1);
}

TEST(server_close_then_rehost)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	std::vector<std::unique_ptr<collector>> cols;
	std::vector<sock_ptr>                   cs(4);
	for (auto& c : cs) {
		CHECK_EQ(datapath::connect(c, name), error::Success);
		cols.push_back(std::make_unique<collector>());
		cols.back()->attach(*c);
		bytes p(256 << 10);
		c->send(p.data(), p.size());
	}
	CHECK_EQ(srv.server->close(), error::Success);
	for (auto& col : cols)
		CHECK(col->closed.wait(1));
	echo_server again;
	CHECK_EQ(again.start(name), error::Success);
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
}

TEST(bad_handshake_rejected)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto p = raw::connect(name);
	CHECK(p != nullptr);
	CHECK(raw::write(*p, "GARBAGE-GARBAGE!", 16));
	CHECK(raw::wait_closed(*p, 5000));
	CHECK_EQ(srv.accepted.value(), 0);
	// Others are still served.
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	CHECK(srv.accepted.wait(1));
}

static raw::peer_ptr raw_handshake(const std::string& name, uint64_t limit)
{
	auto p = raw::connect(name);
	if (!p)
		return nullptr;
	uint8_t hello[datapath::detail::hello_size];
	datapath::detail::encode_hello(hello, {datapath::detail::protocol_version, limit});
	if (!raw::write(*p, hello, sizeof(hello)))
		return nullptr;
	uint8_t reply[datapath::detail::hello_size];
	if (!raw::read(*p, reply, sizeof(reply), 5000))
		return nullptr;
	return p;
}

TEST(peer_dies_mid_frame)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto p = raw_handshake(name, 1 << 20);
	CHECK(p != nullptr);
	CHECK(srv.accepted.wait(1));
	uint8_t frame[datapath::detail::frame_header_size + 4];
	datapath::detail::encode_frame_header(frame, 100);
	std::memcpy(frame + 8, "half", 4);
	CHECK(raw::write(*p, frame, sizeof(frame)));
	p.reset();
	CHECK(srv.closed.wait(1));
}

TEST(oversized_header_disconnects)
{
	auto              name = unique_name();
	echo_server       srv;
	datapath::options so;
	so.max_message_size = 1024;
	CHECK_EQ(srv.start(name, so), error::Success);
	auto p = raw_handshake(name, 1 << 20);
	CHECK(p != nullptr);
	CHECK(srv.accepted.wait(1));
	uint8_t header[datapath::detail::frame_header_size];
	datapath::detail::encode_frame_header(header, uint64_t(1) << 40);
	CHECK(raw::write(*p, header, sizeof(header)));
	CHECK(raw::wait_closed(*p, 5000));
	CHECK(srv.closed.wait(1));
}

TEST(custom_context_shutdown_closes_sockets)
{
	auto ctx  = datapath::context::create({std::chrono::microseconds(0), 2});
	auto name = unique_name();
	srv_ptr server;
	CHECK_EQ(datapath::host(server, name, {}, ctx), error::Success);
	sock_ptr keep;
	server->on_accept([&](sock_ptr s) {
		keep = s;
		return true;
	});
	collector col;
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	col.attach(*c);
	ctx.reset(); // last reference: shuts the context down
	CHECK(col.closed.wait(1));
	CHECK(!keep || !keep->good());
}

// io_threads = 0: one I/O thread per physical core. Several connections spread
// over those threads must all echo correctly.
TEST(auto_io_threads_echo)
{
	datapath::context_options co;
	co.spin       = std::chrono::microseconds(0);
	co.io_threads = 0;
	auto ctx      = datapath::context::create(co);
	CHECK(ctx != nullptr);
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	const size_t           n = 8;
	std::vector<collector> cols(n);
	std::vector<sock_ptr>  socks(n);
	for (size_t i = 0; i < n; i++) {
		CHECK_EQ(datapath::connect(socks[i], name, {}, ctx), error::Success);
		cols[i].attach(*socks[i]);
		auto msg = pattern(64 + i, uint32_t(i));
		CHECK_EQ(socks[i]->send(msg.data(), msg.size()), error::Success);
	}
	for (size_t i = 0; i < n; i++) {
		CHECK(cols[i].received.wait(1));
		std::lock_guard<std::mutex> l(cols[i].lock);
		CHECK(cols[i].messages[0] == pattern(64 + i, uint32_t(i)));
	}
}

TEST(drop_context_inside_callback)
{
	struct holder {
		std::shared_ptr<datapath::context> ctx;
		srv_ptr                            server;
		sock_ptr                           server_side;
		sock_ptr                           client;
	};
	auto name = unique_name();
	auto h    = std::make_shared<holder>();
	h->ctx    = datapath::context::create({std::chrono::microseconds(0), 1});
	latch accepted, done;
	CHECK_EQ(datapath::host(h->server, name, {}, h->ctx), error::Success);
	holder* hraw = h.get();
	h->server->on_accept([hraw, &accepted](sock_ptr s) {
		hraw->server_side = s;
		accepted.notify();
		return true;
	});
	CHECK_EQ(datapath::connect(h->client, name, {}, h->ctx), error::Success);
	CHECK(accepted.wait(1));
	auto* slot = new std::shared_ptr<holder>(std::move(h));
	(*slot)->client->on_message([slot, &done](const uint8_t*, size_t) {
		// Drop every handle, including the last context reference, on an I/O thread.
		auto local = std::move(*slot);
		local.reset();
		done.notify();
	});
	// Keep our own reference while sending: send() writes inline, and the
	// callback may drop the holder (and with it every other reference) on the
	// I/O thread before send() has returned.
	sock_ptr sender = (*slot)->server_side;
	sender->send("x", 1);
	CHECK(done.wait(1));
	sender.reset();
	std::this_thread::sleep_for(300ms); // let the context finish tearing down
	delete slot;
}

TEST(bad_version_gets_server_hello)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto p = raw::connect(name);
	CHECK(p != nullptr);
	uint8_t hello[datapath::detail::hello_size];
	datapath::detail::encode_hello(hello, {datapath::detail::protocol_version + 7, 1024});
	CHECK(raw::write(*p, hello, sizeof(hello)));
	// The server answers with its own hello so the client can report VersionMismatch.
	uint8_t reply[datapath::detail::hello_size];
	CHECK(raw::read(*p, reply, sizeof(reply), 5000));
	datapath::detail::hello h{};
	CHECK_EQ(datapath::detail::decode_hello(reply, h), error::Success);
	CHECK(raw::wait_closed(*p, 5000));
	CHECK_EQ(srv.accepted.value(), 0);
}

TEST(rehost_while_old_client_holds_handle)
{
	auto name = unique_name();
	auto srv  = std::make_unique<echo_server>();
	CHECK_EQ(srv->start(name), error::Success);
	auto p = raw_handshake(name, 1 << 20); // never reads, never closes
	CHECK(p != nullptr);
	CHECK(srv->accepted.wait(1));
	srv.reset();
	echo_server again;
	CHECK_EQ(again.start(name), error::Success);
}

TEST_MAIN()
