/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// The C API (datapath.h): option handling, argument checks, callbacks and
// handle ownership. c_api_echo.c covers use from actual C.

#include <datapath/datapath.h>
#include <datapath/datapath.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "check.hpp"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

extern "C" int c_api_echo_round_trip(const char* path);

using namespace std::chrono_literals;

static std::string unique_name()
{
	static std::atomic<int> counter{0};
	return "dp-ctest-" + std::to_string(getpid()) + "-" + std::to_string(counter++);
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
};

// Echo server built on the C API; keeps every accepted client.
struct c_echo_server {
	latch                         accepted;
	latch                         closed;
	std::mutex                    lock;
	std::vector<datapath_socket*> clients;
	datapath_server*              server = nullptr;

	static void echo(void*, datapath_socket* s, const uint8_t* d, size_t n)
	{
		datapath_socket_send(s, d, n);
	}
	static void on_close(void* user, datapath_socket*, datapath_error)
	{
		static_cast<c_echo_server*>(user)->closed.notify();
	}
	static int on_accept(void* user, datapath_server*, datapath_socket* client)
	{
		auto* self = static_cast<c_echo_server*>(user);
		datapath_socket_set_on_close(client, on_close, self, nullptr);
		datapath_socket_set_on_message(client, echo, nullptr, nullptr);
		{
			std::lock_guard<std::mutex> l(self->lock);
			self->clients.push_back(client);
		}
		self->accepted.notify();
		return 1;
	}

	datapath_error start(const std::string& name, const datapath_options* opts = nullptr)
	{
		auto ec = datapath_host(name.c_str(), opts, nullptr, &server);
		if (ec != DATAPATH_SUCCESS)
			return ec;
		return datapath_server_set_on_accept(server, on_accept, this, nullptr);
	}

	~c_echo_server()
	{
		datapath_server_release(server);
		for (auto* c : clients)
			datapath_socket_release(c);
	}
};

struct collector {
	std::mutex                        lock;
	std::vector<std::vector<uint8_t>> messages;
	latch                             received;
	latch                             closed;
	std::atomic<int>                  close_calls{0};
	std::atomic<int32_t>              reason{DATAPATH_ERROR_UNKNOWN};

	static void on_message(void* user, datapath_socket*, const uint8_t* d, size_t n)
	{
		auto* self = static_cast<collector*>(user);
		{
			std::lock_guard<std::mutex> l(self->lock);
			self->messages.emplace_back(d, d + n);
		}
		self->received.notify();
	}
	static void on_close(void* user, datapath_socket*, datapath_error reason)
	{
		auto* self   = static_cast<collector*>(user);
		self->reason = reason;
		self->close_calls++;
		self->closed.notify();
	}
	void attach(datapath_socket* s)
	{
		datapath_socket_set_on_close(s, on_close, this, nullptr);
		datapath_socket_set_on_message(s, on_message, this, nullptr);
	}
};

TEST(error_strings)
{
	using datapath::error;
	std::set<std::string> seen;
	for (int32_t e = DATAPATH_ERROR_UNKNOWN; e <= DATAPATH_ERROR_OUT_OF_MEMORY; e++) {
		const char* s = datapath_error_string(e);
		CHECK(s != nullptr);
		CHECK(s[0] != '\0');
		seen.insert(s);
		if (e <= DATAPATH_ERROR_ACCESS_DENIED)
			CHECK_EQ(std::string(s), std::string(datapath::to_string(error(e))));
	}
	CHECK_EQ(seen.size(), size_t(DATAPATH_ERROR_OUT_OF_MEMORY + 2));
	CHECK_EQ(std::string(datapath_error_string(9999)), std::string("Invalid"));
}

TEST(version)
{
	CHECK_EQ(datapath_c_abi_version(), uint32_t(DATAPATH_C_ABI_VERSION));
	CHECK(datapath_version() != 0);
}

TEST(options_defaults_match_cpp)
{
	datapath_context_options co;
	std::memset(&co, 0xAB, sizeof(co));
	datapath_context_options_init(&co);
	datapath::context_options cco;
	CHECK_EQ(co.struct_size, uint32_t(sizeof(co)));
	CHECK_EQ(co.spin_us, int64_t(cco.spin.count()));
	CHECK_EQ(co.io_threads, uint64_t(cco.io_threads));
	CHECK_EQ(co.priority, cco.priority);
	CHECK_EQ(co.ideal_core, cco.ideal_core);
	CHECK_EQ(int(co.mmcss), int(cco.mmcss));
	CHECK_EQ(int(co.alertable), int(cco.alertable));
	CHECK_EQ(int(co.yield), int(cco.yield));
	CHECK_EQ(int(co.pair_peer), int(cco.pair_peer));

	datapath_options o;
	datapath_options_init(&o);
	datapath::options oo;
	CHECK_EQ(o.struct_size, uint32_t(sizeof(o)));
	CHECK_EQ(o.permissions, uint32_t(DATAPATH_PERMISSIONS_USER));
	CHECK_EQ(o.max_message_size, oo.max_message_size);
	CHECK_EQ(o.max_queued_bytes, oo.max_queued_bytes);
	CHECK_EQ(o.backlog, uint64_t(oo.backlog));
	CHECK_EQ(o.max_clients, uint64_t(oo.max_clients));
	CHECK_EQ(o.connect_timeout_ms, int64_t(oo.connect_timeout.count()));
	CHECK_EQ(o.receive_buffer, uint64_t(oo.receive_buffer));
	CHECK_EQ(o.pipe_buffer, uint64_t(oo.pipe_buffer));
	CHECK_EQ(o.handshake_timeout_ms, int64_t(oo.handshake_timeout.count()));

	datapath_context_options_init(nullptr);
	datapath_options_init(nullptr);
}

TEST(invalid_arguments)
{
	datapath_socket*  s = nullptr;
	datapath_server*  v = nullptr;
	datapath_context* c = nullptr;
	CHECK_EQ(datapath_connect(nullptr, nullptr, nullptr, &s), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_connect("x", nullptr, nullptr, nullptr), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_host(nullptr, nullptr, nullptr, &v), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_host("x", nullptr, nullptr, nullptr), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_context_create(nullptr, nullptr), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_context_get_default(nullptr), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_socket_send(nullptr, "x", 1), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_socket_sendv(nullptr, nullptr, 0), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_socket_close(nullptr), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_server_close(nullptr), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_socket_good(nullptr), 0);
	CHECK_EQ(datapath_socket_max_message_size(nullptr), uint64_t(0));
	CHECK_EQ(datapath_socket_set_zero_copy(nullptr, 1), 0);
	datapath_socket_release(nullptr);
	datapath_server_release(nullptr);
	datapath_context_release(nullptr);

	// User data handed over with a failing call is still freed.
	static int freed = 0;
	freed            = 0;
	datapath_free_fn count_free = [](void*) { freed++; };
	CHECK_EQ(datapath_socket_set_on_message(nullptr, nullptr, nullptr, count_free), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_socket_set_on_close(nullptr, nullptr, nullptr, count_free), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(datapath_server_set_on_accept(nullptr, nullptr, nullptr, count_free), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK_EQ(freed, 3);

	datapath_options o;
	datapath_options_init(&o);
	o.permissions = 7;
	CHECK_EQ(datapath_connect("x", &o, nullptr, &s), DATAPATH_ERROR_INVALID_ARGUMENT);
	datapath_options_init(&o);
	o.handshake_timeout_ms = -1;
	CHECK_EQ(datapath_host("x", &o, nullptr, &v), DATAPATH_ERROR_INVALID_ARGUMENT);
	datapath_options_init(&o);
	o.struct_size = 0;
	CHECK_EQ(datapath_host("x", &o, nullptr, &v), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK(s == nullptr);
	CHECK(v == nullptr);

	datapath_context_options co;
	datapath_context_options_init(&co);
	co.priority = 9;
	CHECK_EQ(datapath_context_create(&co, &c), DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK(c == nullptr);
}

TEST(contexts)
{
	datapath_context *a = nullptr, *b = nullptr;
	CHECK_EQ(datapath_context_get_default(&a), DATAPATH_SUCCESS);
	CHECK_EQ(datapath_context_get_default(&b), DATAPATH_SUCCESS);
	CHECK(a != nullptr && b != nullptr);
	datapath_context_release(a);
	datapath_context_release(b);

	datapath_context_options co;
	datapath_context_options_init(&co);
	co.spin_us  = 0;
	co.priority = 0;
	datapath_context* own = nullptr;
	CHECK_EQ(datapath_context_create(&co, &own), DATAPATH_SUCCESS);

	// A server and client on a private context.
	std::string      name   = unique_name();
	datapath_server* server = nullptr;
	CHECK_EQ(datapath_host(name.c_str(), nullptr, own, &server), DATAPATH_SUCCESS);
	static std::atomic<int> accepted;
	accepted = 0;
	datapath_server_set_on_accept(
		server, [](void*, datapath_server*, datapath_socket*) { return accepted++, 0; }, nullptr, nullptr);
	datapath_socket* client = nullptr;
	// Rejected clients see Closed.
	CHECK_EQ(datapath_connect(name.c_str(), nullptr, own, &client), DATAPATH_ERROR_CLOSED);
	CHECK(client == nullptr);
	CHECK_EQ(accepted.load(), 1);
	datapath_server_release(server);
	datapath_context_release(own);
}

TEST(older_smaller_options_struct)
{
	// A caller built against a header that only had the first two fields.
	struct v0 {
		uint32_t struct_size;
		uint32_t permissions;
	} old{sizeof(v0), DATAPATH_PERMISSIONS_USER};
	c_echo_server srv;
	std::string   name = unique_name();
	CHECK_EQ(srv.start(name, reinterpret_cast<const datapath_options*>(&old)), DATAPATH_SUCCESS);
	datapath_socket* s = nullptr;
	CHECK_EQ(datapath_connect(name.c_str(), reinterpret_cast<const datapath_options*>(&old), nullptr, &s),
			 DATAPATH_SUCCESS);
	CHECK_EQ(datapath_socket_max_message_size(s), datapath::options{}.max_message_size);
	datapath_socket_release(s);
}

TEST(echo_from_c)
{
	CHECK_EQ(c_api_echo_round_trip(unique_name().c_str()), 0);
}

TEST(echo_and_close)
{
	c_echo_server srv;
	std::string   name = unique_name();
	CHECK_EQ(srv.start(name), DATAPATH_SUCCESS);

	datapath_options o;
	datapath_options_init(&o);
	o.max_message_size = 1024;
	datapath_socket* s = nullptr;
	CHECK_EQ(datapath_connect(name.c_str(), &o, nullptr, &s), DATAPATH_SUCCESS);
	CHECK(srv.accepted.wait(1));
	CHECK_EQ(datapath_socket_good(s), 1);
	CHECK_EQ(datapath_socket_max_message_size(s), uint64_t(1024));

	collector col;
	col.attach(s);
	for (int i = 0; i < 100; i++) {
		uint32_t v = uint32_t(i);
		CHECK_EQ(datapath_socket_send(s, &v, sizeof(v)), DATAPATH_SUCCESS);
	}
	CHECK(col.received.wait(100));
	{
		std::lock_guard<std::mutex> l(col.lock);
		for (int i = 0; i < 100; i++) {
			uint32_t v;
			CHECK_EQ(col.messages[i].size(), sizeof(v));
			std::memcpy(&v, col.messages[i].data(), sizeof(v));
			CHECK_EQ(v, uint32_t(i));
		}
	}

	std::vector<uint8_t> big(2048);
	CHECK_EQ(datapath_socket_send(s, big.data(), big.size()), DATAPATH_ERROR_TOO_LARGE);
	CHECK_EQ(datapath_socket_send(s, nullptr, 0), DATAPATH_SUCCESS);
	CHECK_EQ(datapath_socket_send(s, nullptr, 1), DATAPATH_ERROR_INVALID_ARGUMENT);

	CHECK_EQ(datapath_socket_close(s), DATAPATH_SUCCESS);
	CHECK(col.closed.wait(1));
	CHECK_EQ(col.close_calls.load(), 1);
	CHECK_EQ(col.reason.load(), DATAPATH_ERROR_CLOSED);
	CHECK_EQ(datapath_socket_good(s), 0);
	CHECK_EQ(datapath_socket_send(s, "x", 1), DATAPATH_ERROR_CLOSED);
	CHECK(srv.closed.wait(1));
	datapath_socket_release(s);
}

TEST(not_found)
{
	datapath_socket* s = nullptr;
	CHECK_EQ(datapath_connect(unique_name().c_str(), nullptr, nullptr, &s), DATAPATH_ERROR_NOT_FOUND);
	CHECK(s == nullptr);
}

TEST(address_in_use)
{
	c_echo_server a;
	std::string   name = unique_name();
	CHECK_EQ(a.start(name), DATAPATH_SUCCESS);
	datapath_server* b = nullptr;
	CHECK_EQ(datapath_host(name.c_str(), nullptr, nullptr, &b), DATAPATH_ERROR_ADDRESS_IN_USE);
	CHECK(b == nullptr);
}

// user_free runs exactly once per callback: on replacement, and on release.
TEST(user_data_freed)
{
	c_echo_server srv;
	std::string   name = unique_name();
	CHECK_EQ(srv.start(name), DATAPATH_SUCCESS);
	datapath_socket* s = nullptr;
	CHECK_EQ(datapath_connect(name.c_str(), nullptr, nullptr, &s), DATAPATH_SUCCESS);

	static std::atomic<int> freed;
	freed                = 0;
	auto free_fn         = [](void* p) {
        freed++;
        delete static_cast<int*>(p);
	};
	auto noop_message    = [](void*, datapath_socket*, const uint8_t*, size_t) {};
	auto noop_close      = [](void*, datapath_socket*, datapath_error) {};
	CHECK_EQ(datapath_socket_set_on_message(s, noop_message, new int(1), free_fn), DATAPATH_SUCCESS);
	CHECK_EQ(freed.load(), 0);
	CHECK_EQ(datapath_socket_set_on_message(s, noop_message, new int(2), free_fn), DATAPATH_SUCCESS);
	CHECK_EQ(freed.load(), 1);
	CHECK_EQ(datapath_socket_set_on_close(s, noop_close, new int(3), free_fn), DATAPATH_SUCCESS);
	// Clearing with NULL drops the old callback, and frees data passed with it.
	CHECK_EQ(datapath_socket_set_on_close(s, nullptr, new int(4), free_fn), DATAPATH_SUCCESS);
	CHECK_EQ(freed.load(), 3);
	datapath_socket_release(s);
	CHECK_EQ(freed.load(), 4);

	CHECK_EQ(datapath_server_set_on_accept(
				 srv.server, [](void*, datapath_server*, datapath_socket*) { return 0; }, new int(5), free_fn),
			 DATAPATH_SUCCESS);
	datapath_server_release(srv.server);
	srv.server = nullptr;
	CHECK_EQ(freed.load(), 5);
}

// The close callback fires on peer disconnect; release from a callback is allowed.
TEST(peer_close_and_release_in_callback)
{
	c_echo_server srv;
	std::string   name = unique_name();
	CHECK_EQ(srv.start(name), DATAPATH_SUCCESS);
	datapath_socket* s = nullptr;
	CHECK_EQ(datapath_connect(name.c_str(), nullptr, nullptr, &s), DATAPATH_SUCCESS);
	CHECK(srv.accepted.wait(1));

	static latch            done;
	static std::atomic<int> freed;
	freed = 0;
	datapath_socket_set_on_close(
		s,
		[](void*, datapath_socket* self, datapath_error) {
			datapath_socket_release(self);
			done.notify();
		},
		nullptr, [](void*) { freed++; });
	datapath_socket_set_on_message(s, [](void*, datapath_socket*, const uint8_t*, size_t) {}, nullptr, nullptr);

	// The server drops its side.
	{
		std::lock_guard<std::mutex> l(srv.lock);
		datapath_socket_release(srv.clients[0]);
		srv.clients.clear();
	}
	CHECK(done.wait(1));
	for (int i = 0; i < 1000 && freed.load() == 0; i++)
		std::this_thread::sleep_for(1ms);
	CHECK_EQ(freed.load(), 1);
}

TEST_MAIN()
