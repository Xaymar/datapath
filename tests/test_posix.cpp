/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Unix domain socket behavior shared by Linux and macOS, plus the parts of
// each platform that differ (paths, thread priority).

#include <datapath/datapath.hpp>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif
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
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include "check.hpp"

using datapath::error;

static std::string socket_path(const char* tag)
{
	return "/tmp/datapath-test-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

TEST(permissions_mode_bits)
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
		if (c.level == datapath::permissions::Group)
			CHECK_EQ(st.st_gid, ::getegid()); // not the directory's group (BSD default)
		srv.reset();
		CHECK(::access(path.c_str(), F_OK) != 0); // removed on close
	}
}

TEST(filesystem_socket_roundtrip_and_stale_file)
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

TEST(bare_name_mapping)
{
	// Linux: abstract socket, no file. macOS: /tmp/datapath.<name>, removed on close.
	std::string                       name = "dp-test-map-" + std::to_string(getpid());
	std::string                       file = "/tmp/datapath." + name;
	std::shared_ptr<datapath::server> srv;
	CHECK_EQ(datapath::host(srv, name), error::Success);
	struct stat st {};
#ifdef __APPLE__
	CHECK_EQ(::stat(file.c_str(), &st), 0);
	CHECK(S_ISSOCK(st.st_mode));
	CHECK_EQ(st.st_mode & 0777, mode_t(0600));
#else
	CHECK(::stat(file.c_str(), &st) != 0);
#endif
	std::shared_ptr<datapath::socket> keep;
	srv->on_accept([&](std::shared_ptr<datapath::socket> s) {
		keep = s;
		return true;
	});
	std::shared_ptr<datapath::socket> c;
	CHECK_EQ(datapath::connect(c, name), error::Success);
	c.reset();
	keep.reset();
	srv.reset();
	CHECK(::stat(file.c_str(), &st) != 0);
	// Too long for sockaddr_un once mapped.
	CHECK_EQ(datapath::host(srv, std::string(sizeof(sockaddr_un::sun_path), 'n')), error::InvalidPath);
}

TEST(connect_is_bounded_when_backlog_full)
{
	// A server that never accepts: fill its backlog, then connect must time out.
	// (Linux reports a full backlog as EAGAIN, macOS as ECONNREFUSED.)
	auto        path = socket_path("backlog");
	int         lfd  = ::socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un a{};
	a.sun_family = AF_UNIX;
	std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
	socklen_t len = socklen_t(sizeof(a));
	CHECK_EQ(::bind(lfd, reinterpret_cast<sockaddr*>(&a), len), 0);
	CHECK_EQ(::listen(lfd, 0), 0);
	std::vector<int> fillers;
	for (int i = 0; i < 64; i++) {
		int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		::fcntl(fd, F_SETFL, O_NONBLOCK);
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
	error ec    = datapath::connect(c, path, o);
	auto  took  = std::chrono::steady_clock::now() - start;
	CHECK_EQ(ec, error::TimedOut);
	CHECK(took < std::chrono::seconds(2));
	for (int fd : fillers)
		::close(fd);
	::close(lfd);
	::unlink(path.c_str());
}

#ifdef __APPLE__
// macOS schedules by quality-of-service class instead of nice values.
static int thread_priority_marker()
{
	return int(qos_class_self());
}
#else
static int thread_priority_marker()
{
	return ::getpriority(PRIO_PROCESS, id_t(::gettid()));
}
#endif

// Scheduling marker of the I/O thread that accepts a connection on a context
// with the given priority.
static int io_thread_marker(int priority)
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
	std::atomic<int>                  seen{-1000};
	std::shared_ptr<datapath::socket> keep;
	srv->on_accept([&](std::shared_ptr<datapath::socket> s) {
		keep = s;
		seen = thread_priority_marker();
		return true;
	});
	std::shared_ptr<datapath::socket> c;
	CHECK_EQ(datapath::connect(c, path, {}, ctx), error::Success);
	for (int i = 0; i < 1000 && seen == -1000; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	keep.reset();
	c.reset();
	srv.reset();
	return seen;
}

TEST(io_thread_priority)
{
	// Priority 0 leaves a new thread as it is.
	int fresh = 0;
	std::thread([&] { fresh = thread_priority_marker(); }).join();
	CHECK_EQ(io_thread_marker(0), fresh);
#ifdef __APPLE__
	CHECK_EQ(io_thread_marker(1), int(QOS_CLASS_USER_INITIATED));
	CHECK_EQ(io_thread_marker(2), int(QOS_CLASS_USER_INTERACTIVE));
	CHECK_EQ(io_thread_marker(3), int(QOS_CLASS_USER_INTERACTIVE));
#else
	int base = fresh;
	// Whether this process may lower a thread's nice value at all depends on
	// RLIMIT_NICE and CAP_SYS_NICE: probe it on a scratch thread.
	int  want = std::max(-20, base - 12);
	bool can  = false;
	std::thread([&] { can = ::setpriority(PRIO_PROCESS, id_t(::gettid()), want) == 0; }).join();
	int got = io_thread_marker(2);
	CHECK(got <= base);
	if (can)
		CHECK_EQ(got, want);
	std::printf("  base nice %d, priority 2 I/O thread nice %d (%s)\n", base, got,
				can ? "lowering allowed" : "lowering not allowed");
#endif
}

TEST_MAIN()
