/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include <datapath/datapath.hpp>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <unistd.h>
#include <string>
#include "check.hpp"

using datapath::error;

static std::string socket_path(const char* tag)
{
	return "/tmp/datapath-test-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

TEST(linux_permissions_mode_bits)
{
	struct {
		datapath::permissions level;
		mode_t                mode;
	} cases[] = {{datapath::permissions::User, 0600},
				 {datapath::permissions::Group, 0660},
				 {datapath::permissions::World, 0666}};
	for (auto& c : cases) {
		auto              path = socket_path("perm");
		datapath::options o;
		o.permissions = c.level;
		std::shared_ptr<datapath::server> srv;
		CHECK_EQ(datapath::host(srv, path, o), error::Success);
		struct stat st {};
		CHECK_EQ(::stat(path.c_str(), &st), 0);
		CHECK(S_ISSOCK(st.st_mode));
		CHECK_EQ(st.st_mode & 0777, c.mode);
		srv.reset();
		CHECK(::access(path.c_str(), F_OK) != 0); // removed on close
	}
}

TEST(linux_filesystem_socket_roundtrip_and_stale_file)
{
	auto path = socket_path("fs");
	{
		// Leave a stale socket file behind, as a crashed server would.
		int         fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		sockaddr_un a{};
		a.sun_family = AF_UNIX;
		std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
		CHECK_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
		::close(fd);
		CHECK_EQ(::access(path.c_str(), F_OK), 0);
	}
	std::shared_ptr<datapath::server> srv;
	CHECK_EQ(datapath::host(srv, path), error::Success);
	std::shared_ptr<datapath::server> second;
	CHECK_EQ(datapath::host(second, path), error::AddressInUse); // live server is not stolen
	std::shared_ptr<datapath::socket> keep;
	srv->on_accept([&](std::shared_ptr<datapath::socket> s) {
		keep = s;
		return true;
	});
	std::shared_ptr<datapath::socket> c;
	CHECK_EQ(datapath::connect(c, path), error::Success);
	CHECK(c->good());
}

TEST(invalid_paths)
{
	std::shared_ptr<datapath::server> srv;
	CHECK_EQ(datapath::host(srv, ""), error::InvalidPath);
	CHECK_EQ(datapath::host(srv, "/" + std::string(200, 'x')), error::InvalidPath);
	CHECK_EQ(datapath::host(srv, std::string(200, 'x')), error::InvalidPath);
}

TEST(host_never_deletes_regular_file)
{
	auto path = socket_path("regular");
	FILE* f   = std::fopen(path.c_str(), "w");
	CHECK(f != nullptr);
	std::fputs("precious", f);
	std::fclose(f);
	std::shared_ptr<datapath::server> srv;
	CHECK_EQ(datapath::host(srv, path), error::AddressInUse);
	struct stat st {};
	CHECK_EQ(::stat(path.c_str(), &st), 0);
	CHECK(S_ISREG(st.st_mode));
	::unlink(path.c_str());
}

TEST(connect_is_bounded_when_backlog_full)
{
	// A server that never accepts: fill its backlog, then connect must time out.
	std::string name = "dp-test-backlog-" + std::to_string(getpid());
	int         lfd  = ::socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un a{};
	a.sun_family     = AF_UNIX;
	std::string abs  = "datapath." + name;
	std::memcpy(a.sun_path + 1, abs.data(), abs.size());
	socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + abs.size());
	CHECK_EQ(::bind(lfd, reinterpret_cast<sockaddr*>(&a), len), 0);
	CHECK_EQ(::listen(lfd, 0), 0);
	std::vector<int> fillers;
	for (int i = 0; i < 64; i++) {
		int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
		if (::connect(fd, reinterpret_cast<sockaddr*>(&a), len) != 0) {
			::close(fd);
			break;
		}
		fillers.push_back(fd);
	}
	datapath::options o;
	o.connect_timeout = std::chrono::milliseconds(200);
	std::shared_ptr<datapath::socket> c;
	auto  start = std::chrono::steady_clock::now();
	error ec    = datapath::connect(c, name, o);
	auto  took  = std::chrono::steady_clock::now() - start;
	CHECK_EQ(ec, error::TimedOut);
	CHECK(took < std::chrono::seconds(2));
	for (int fd : fillers)
		::close(fd);
	::close(lfd);
}

static int thread_nice()
{
	return ::getpriority(PRIO_PROCESS, id_t(::gettid()));
}

// Nice value of the I/O thread that accepts a connection on a context with
// the given priority.
static int io_thread_nice(int priority)
{
	datapath::context_options co;
	co.spin       = std::chrono::microseconds(0);
	co.io_threads = 1;
	co.priority   = priority;
	auto ctx      = datapath::context::create(co);
	CHECK(ctx != nullptr);
	auto                              path = socket_path("prio");
	std::shared_ptr<datapath::server> srv;
	CHECK_EQ(datapath::host(srv, path, {}, ctx), error::Success);
	std::atomic<int>                  seen{100};
	std::shared_ptr<datapath::socket> keep;
	srv->on_accept([&](std::shared_ptr<datapath::socket> s) {
		keep = s;
		seen = thread_nice();
		return true;
	});
	std::shared_ptr<datapath::socket> c;
	CHECK_EQ(datapath::connect(c, path, {}, ctx), error::Success);
	for (int i = 0; i < 1000 && seen == 100; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	keep.reset();
	c.reset();
	srv.reset();
	return seen;
}

TEST(linux_io_thread_priority)
{
	int base = thread_nice();
	CHECK_EQ(io_thread_nice(0), base);
	// Whether this process may lower a thread's nice value at all depends on
	// RLIMIT_NICE and CAP_SYS_NICE: probe it on a scratch thread.
	int  want = std::max(-20, base - 12);
	bool can  = false;
	std::thread([&] { can = ::setpriority(PRIO_PROCESS, id_t(::gettid()), want) == 0; }).join();
	int got = io_thread_nice(2);
	CHECK(got <= base);
	if (can)
		CHECK_EQ(got, want);
	std::printf("  base nice %d, priority 2 I/O thread nice %d (%s)\n", base, got,
				can ? "lowering allowed" : "lowering not allowed");
}

TEST_MAIN()
