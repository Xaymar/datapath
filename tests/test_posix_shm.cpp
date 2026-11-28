/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// options::shared_memory on Linux: the version 3 handshake, the descriptors it
// passes, the data path itself, and what happens when the peer's process ends.
// The rest of the integration suite also runs over shared memory (*.shm).

#include <datapath/datapath.hpp>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "check.hpp"
#include "common/endian.hpp"
#include "common/handshake.hpp"
#include "common/shm_ring.hpp"
#include "posix/path.hpp"
#include "posix/shm.hpp"
#include "posix/uds_connection.hpp"

extern char** environ;

using datapath::error;
using sock_ptr = std::shared_ptr<datapath::socket>;
using srv_ptr  = std::shared_ptr<datapath::server>;

namespace {
	std::string unique_name()
	{
		static std::atomic<int> n{0};
		return "dp-test-shm-" + std::to_string(getpid()) + "-" + std::to_string(n++);
	}

	datapath::options shm_options()
	{
		datapath::options o;
		o.shared_memory = true;
		return o;
	}

	datapath::options socket_options()
	{
		datapath::options o;
		o.shared_memory = false;
		return o;
	}

	// Whether `p` points into one of this process's shared-memory sections.
	bool in_shm_section(const void* p)
	{
		auto  a = reinterpret_cast<uintptr_t>(p);
		FILE* f = std::fopen("/proc/self/maps", "r");
		if (!f)
			return false;
		char line[512];
		bool hit = false;
		while (!hit && std::fgets(line, sizeof(line), f)) {
			unsigned long lo = 0, hi = 0;
			if (std::strstr(line, "memfd:datapath-shm") && std::sscanf(line, "%lx-%lx", &lo, &hi) == 2)
				hit = a >= lo && a < hi;
		}
		std::fclose(f);
		return hit;
	}

	struct waiter {
		std::mutex              lock;
		std::condition_variable cv;
		int                     count = 0;
		void                    hit()
		{
			std::lock_guard<std::mutex> l(lock);
			count++;
			cv.notify_all();
		}
		bool wait(int n, int seconds = 5)
		{
			std::unique_lock<std::mutex> l(lock);
			return cv.wait_for(l, std::chrono::seconds(seconds), [&] { return count >= n; });
		}
	};

	std::shared_ptr<datapath::posix::uds_connection> connect_internal(const std::string& name,
																	  const datapath::options& o, error* ec = nullptr)
	{
		std::shared_ptr<datapath::detail::connection> conn;
		auto e = datapath::context::get_default()->impl()->connect(conn, name, o);
		if (ec)
			*ec = e;
		if (e != error::Success)
			return nullptr;
		return std::dynamic_pointer_cast<datapath::posix::uds_connection>(conn);
	}

	struct echo_server {
		// Declared first, destroyed last: callbacks may still use them while the
		// sockets below are released.
		waiter                accepted, closed;
		std::atomic<int>      close_reason{-1};
		std::mutex            lock;
		srv_ptr               server;
		std::vector<sock_ptr> clients;

		error start(const std::string& name, const datapath::options& o = shm_options())
		{
			auto ec = datapath::host(server, name, o);
			if (ec != error::Success)
				return ec;
			server->on_accept([this](sock_ptr s) {
				datapath::socket* raw = s.get();
				s->on_message([raw](const uint8_t* d, size_t n) { raw->send(d, n); });
				s->on_close([this](error e) {
					close_reason = int(e);
					closed.hit();
				});
				{
					std::lock_guard<std::mutex> l(lock);
					clients.push_back(std::move(s));
				}
				accepted.hit();
				return true;
			});
			return error::Success;
		}
	};

	// A listening socket for `name` that tests answer by hand.
	int raw_listen(const std::string& name)
	{
		datapath::posix::socket_address a;
		if (!datapath::posix::make_socket_address(name, a))
			return -1;
		int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&a.addr), a.length) != 0 || ::listen(fd, 4) != 0) {
			if (fd >= 0)
				::close(fd);
			return -1;
		}
		return fd;
	}

	int raw_connect(const std::string& name)
	{
		datapath::posix::socket_address a;
		if (!datapath::posix::make_socket_address(name, a))
			return -1;
		int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&a.addr), a.length) == 0)
			return fd;
		if (fd >= 0)
			::close(fd);
		return -1;
	}

	bool read_all(int fd, void* p, size_t n)
	{
		auto* b = static_cast<uint8_t*>(p);
		while (n) {
			ssize_t r = ::recv(fd, b, n, 0);
			if (r <= 0)
				return false;
			b += r;
			n -= size_t(r);
		}
		return true;
	}

	// Send `data` with `fds` attached.
	bool send_with_fds(int sock, const void* data, size_t size, const int* fds, int count)
	{
		alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int) * 8)] = {};
		iovec                 iov{const_cast<void*>(data), size};
		msghdr                msg{};
		msg.msg_iov        = &iov;
		msg.msg_iovlen     = 1;
		msg.msg_control    = ctl;
		msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);
		cmsghdr* cm        = CMSG_FIRSTHDR(&msg);
		cm->cmsg_level     = SOL_SOCKET;
		cm->cmsg_type      = SCM_RIGHTS;
		cm->cmsg_len       = CMSG_LEN(sizeof(int) * count);
		std::memcpy(CMSG_DATA(cm), fds, sizeof(int) * count);
		return ::sendmsg(sock, &msg, MSG_NOSIGNAL) == ssize_t(size);
	}

	// Child process for peer_process_dies: connect, send one message, then hang
	// until the parent kills this process.
	int run_child(const char* name)
	{
		sock_ptr c;
		if (datapath::connect(c, name, shm_options()) != error::Success)
			return 2;
		c->send("alive", 5);
		for (;;)
			pause();
	}
} // namespace

TEST(options_default_on_with_copy)
{
	datapath::options o;
	CHECK(o.shared_memory);
	CHECK(!o.shared_memory_zero_copy);
	CHECK(o.shared_memory_size > 0);
	CHECK(datapath::posix::shm_link::supported());
}

// Both sides opted in: the connection ends up on shared memory and echoes
// messages of every size class, including ones larger than the ring.
TEST(echo_all_sizes)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto c = connect_internal(name, shm_options());
	CHECK(c != nullptr);
	CHECK(c->shared_memory_active());
	auto sock = std::make_shared<datapath::socket>(c);

	std::mutex                        lock;
	std::vector<std::vector<uint8_t>> got;
	waiter                            w;
	sock->on_message([&](const uint8_t* d, size_t n) {
		{
			std::lock_guard<std::mutex> l(lock);
			got.emplace_back(d, d + n);
		}
		w.hit();
	});
	std::vector<size_t> sizes = {0, 1, 7, 8, 63, 64, 4095, 4096, 4097, 65536, 1 << 20, (4 << 20) + 13, 12 << 20};
	std::vector<std::vector<uint8_t>> sent;
	for (size_t i = 0; i < sizes.size(); i++) {
		std::vector<uint8_t> m(sizes[i]);
		for (size_t j = 0; j < m.size(); j++)
			m[j] = uint8_t(j * 31 + i);
		error ec;
		while ((ec = sock->send(m.data(), m.size())) == error::WouldBlock)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK_EQ(ec, error::Success);
		sent.push_back(std::move(m));
	}
	CHECK(w.wait(int(sizes.size()), 30));
	std::lock_guard<std::mutex> l(lock);
	CHECK_EQ(got.size(), sent.size());
	for (size_t i = 0; i < sent.size(); i++)
		CHECK(got[i] == sent[i]);
}

TEST(ordered_100000)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name, shm_options()), error::Success);
	std::atomic<uint32_t> next{0};
	std::atomic<bool>     bad{false};
	waiter                done;
	const uint32_t        count = 100000;
	c->on_message([&](const uint8_t* d, size_t n) {
		uint32_t v = 0;
		if (n != 4)
			bad = true;
		else
			std::memcpy(&v, d, 4);
		if (v != next.fetch_add(1))
			bad = true;
		if (v + 1 == count)
			done.hit();
	});
	for (uint32_t i = 0; i < count; i++) {
		error ec;
		while ((ec = c->send(&i, 4)) == error::WouldBlock)
			std::this_thread::yield();
		CHECK_EQ(ec, error::Success);
	}
	CHECK(done.wait(1, 30));
	CHECK(!bad.load());
}

// Only one side opted in: the connection works over the socket.
TEST(client_only_uses_socket)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name, socket_options()), error::Success);
	auto c = connect_internal(name, shm_options());
	CHECK(c != nullptr);
	CHECK(!c->shared_memory_active());
	auto   sock = std::make_shared<datapath::socket>(c);
	waiter w;
	sock->on_message([&](const uint8_t*, size_t n) {
		if (n == 4)
			w.hit();
	});
	CHECK_EQ(sock->send("ping", 4), error::Success);
	CHECK(w.wait(1));
}

TEST(server_only_uses_socket)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto c = connect_internal(name, socket_options());
	CHECK(c != nullptr);
	CHECK(!c->shared_memory_active());
	auto   sock = std::make_shared<datapath::socket>(c);
	waiter w;
	sock->on_message([&](const uint8_t*, size_t n) {
		if (n == 4)
			w.hit();
	});
	CHECK_EQ(sock->send("ping", 4), error::Success);
	CHECK(w.wait(1));
}

// A server that only knows version 1 answers a version 3 hello with its own
// hello and closes. The client must connect again with version 1.
TEST(falls_back_to_version_1_server)
{
	auto name = unique_name();
	int  lfd  = raw_listen(name);
	CHECK(lfd >= 0);
	std::vector<uint32_t> versions;
	std::thread           server([&] {
        for (int i = 0; i < 2; i++) {
            int fd = ::accept(lfd, nullptr, nullptr);
            if (fd < 0)
                return;
            uint8_t hello[datapath::detail::hello_size] = {};
            if (!read_all(fd, hello, sizeof(hello))) {
                ::close(fd);
                return;
            }
            datapath::detail::hello in{};
            datapath::detail::decode_hello_any(hello, in);
            versions.push_back(in.version);
            datapath::detail::encode_hello(hello, {datapath::detail::protocol_version, 1024});
            ::send(fd, hello, sizeof(hello), MSG_NOSIGNAL);
            if (in.version != datapath::detail::protocol_version) {
                ::close(fd); // what a version 1 server does after a mismatch
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            ::close(fd);
            return;
        }
	});
	auto c = connect_internal(name, shm_options());
	server.join();
	::close(lfd);
	CHECK(c != nullptr);
	CHECK(!c->shared_memory_active());
	CHECK_EQ(versions.size(), size_t(2));
	CHECK_EQ(versions[0], datapath::detail::protocol_version_ext);
	CHECK_EQ(versions[1], datapath::detail::protocol_version);
}

// The server's side of version 3, seen from a raw client: hello, trailer with
// the shared-memory grant, the 64-byte offer, and five descriptors: a sealed
// memfd of the right size and four eventfds.
TEST(server_answers_version_3_with_offer_and_fds)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	int fd = raw_connect(name);
	CHECK(fd >= 0);
	uint8_t req[datapath::detail::hello_size + datapath::detail::ext_trailer_size] = {};
	datapath::detail::encode_hello(req, {datapath::detail::protocol_version_ext, 1024});
	datapath::detail::encode_ext_trailer(req + datapath::detail::hello_size, datapath::detail::ext_shared_memory, 0);
	CHECK_EQ(::send(fd, req, sizeof(req), MSG_NOSIGNAL), ssize_t(sizeof(req)));

	uint8_t reply[datapath::detail::hello_size + datapath::detail::ext_trailer_size + datapath::posix::shm_offer_size];
	std::vector<int> fds;
	size_t           got = 0;
	while (got < sizeof(reply)) {
		alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int) * 8)] = {};
		iovec                 iov{reply + got, sizeof(reply) - got};
		msghdr                msg{};
		msg.msg_iov        = &iov;
		msg.msg_iovlen     = 1;
		msg.msg_control    = ctl;
		msg.msg_controllen = sizeof(ctl);
		ssize_t n          = ::recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
		CHECK(n > 0);
		for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
			for (size_t i = 0; i < (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int); i++) {
				int f;
				std::memcpy(&f, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
				fds.push_back(f);
			}
		got += size_t(n);
	}
	datapath::detail::hello r{};
	CHECK_EQ(datapath::detail::decode_hello_any(reply, r), error::Success);
	CHECK_EQ(r.version, datapath::detail::protocol_version_ext);
	uint32_t cpu = 0, accepted = 0;
	datapath::detail::decode_ext_trailer(reply + datapath::detail::hello_size, cpu, accepted);
	CHECK_EQ(accepted, datapath::detail::ext_shared_memory);
	const uint8_t* offer = reply + datapath::detail::hello_size + datapath::detail::ext_trailer_size;
	CHECK(std::memcmp(offer, "DPSM", 4) == 0);
	uint64_t cap = datapath::detail::load_le64(offer + 8);

	CHECK_EQ(fds.size(), size_t(datapath::posix::shm_fd_count));
	struct stat st {};
	CHECK_EQ(::fstat(fds[0], &st), 0);
	CHECK(uint64_t(st.st_size) >= datapath::detail::shm::data_offset + 2 * cap);
	int seals = ::fcntl(fds[0], F_GET_SEALS);
	CHECK(seals >= 0 && (seals & F_SEAL_SHRINK) && (seals & F_SEAL_SEAL));
	CHECK(::ftruncate(fds[0], 4096) != 0); // the client can't make the server fault
	for (size_t i = 1; i < fds.size(); i++) {
		char link[64] = {};
		std::string p = "/proc/self/fd/" + std::to_string(fds[i]);
		CHECK(::readlink(p.c_str(), link, sizeof(link) - 1) > 0);
		CHECK(std::strcmp(link, "anon_inode:[eventfd]") == 0);
	}
	for (int f : fds)
		::close(f);
	::close(fd);
	CHECK(srv.closed.wait(1)); // the server's side ends with the raw client
}

// A hostile server hands over a memfd it can still shrink: the client must
// refuse it instead of mapping memory that could raise SIGBUS later, and
// connect again over the plain socket.
TEST(client_refuses_unsealed_section)
{
	auto name = unique_name();
	int  lfd  = raw_listen(name);
	CHECK(lfd >= 0);
	std::vector<uint32_t> versions;
	std::thread           server([&] {
		int fd = ::accept(lfd, nullptr, nullptr);
		if (fd < 0)
			return;
		uint8_t req[datapath::detail::hello_size + datapath::detail::ext_trailer_size];
		if (!read_all(fd, req, sizeof(req))) {
			::close(fd);
			return;
		}
		datapath::detail::hello in{};
		datapath::detail::decode_hello_any(req, in);
		versions.push_back(in.version);
		const uint64_t cap = datapath::detail::shm::min_ring;
		int            fds[datapath::posix::shm_fd_count];
		fds[0] = int(::syscall(SYS_memfd_create, "hostile", MFD_CLOEXEC));
		if (::ftruncate(fds[0], off_t(datapath::detail::shm::data_offset + 2 * cap)) != 0)
			fds[0] = -1;
		for (int i = 1; i < datapath::posix::shm_fd_count; i++)
			fds[i] = ::eventfd(0, EFD_CLOEXEC);
		uint8_t reply[datapath::detail::hello_size + datapath::detail::ext_trailer_size
					  + datapath::posix::shm_offer_size] = {};
		datapath::detail::encode_hello(reply, {datapath::detail::protocol_version_ext, 1024});
		datapath::detail::encode_ext_trailer(reply + datapath::detail::hello_size, uint32_t(-1),
											 datapath::detail::ext_shared_memory);
		uint8_t* offer = reply + datapath::detail::hello_size + datapath::detail::ext_trailer_size;
		std::memcpy(offer, "DPSM", 4);
		datapath::detail::store_le32(offer + 4, datapath::detail::shm::version);
		datapath::detail::store_le64(offer + 8, cap);
		datapath::detail::store_le32(offer + 16, datapath::detail::shm::flag_mirror);
		send_with_fds(fd, reply, sizeof(reply), fds, datapath::posix::shm_fd_count);
		for (int f : fds)
			::close(f);

		// The client gives up on the offer and asks again with version 1.
		int fd2 = ::accept(lfd, nullptr, nullptr);
		if (fd2 >= 0) {
			uint8_t hello[datapath::detail::hello_size] = {};
			if (read_all(fd2, hello, sizeof(hello))) {
				datapath::detail::hello in2{};
				datapath::detail::decode_hello_any(hello, in2);
				versions.push_back(in2.version);
				datapath::detail::encode_hello(hello, {datapath::detail::protocol_version, 1024});
				::send(fd2, hello, sizeof(hello), MSG_NOSIGNAL);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			::close(fd2);
		}
		::close(fd);
	});
	error ec = error::Success;
	auto  c  = connect_internal(name, shm_options(), &ec);
	server.join();
	::close(lfd);
	CHECK_EQ(ec, error::Success);
	CHECK(c != nullptr);
	CHECK(!c->shared_memory_active());
	CHECK_EQ(versions.size(), size_t(2));
	CHECK_EQ(versions[0], datapath::detail::protocol_version_ext);
	CHECK_EQ(versions[1], datapath::detail::protocol_version);
}

// By default a same-user peer's messages reach the handler as a private copy,
// so the peer can't change them while the handler reads; with
// shared_memory_zero_copy they come straight from the ring.
TEST(same_user_messages_copied_unless_zero_copy)
{
	for (bool zero_copy : {false, true}) {
		auto        name = unique_name();
		echo_server srv;
		CHECK_EQ(srv.start(name), error::Success);
		auto o                    = shm_options();
		o.shared_memory_zero_copy = zero_copy;
		auto c                    = connect_internal(name, o);
		CHECK(c != nullptr);
		CHECK(c->shared_memory_active());
		std::atomic<int> in_ring{-1};
		waiter           w; // before the socket whose handler uses them
		auto             sock = std::make_shared<datapath::socket>(c);
		sock->on_message([&](const uint8_t* d, size_t n) {
			if (n == 4) {
				in_ring = in_shm_section(d) ? 1 : 0;
				w.hit();
			}
		});
		CHECK_EQ(sock->send("ping", 4), error::Success);
		CHECK(w.wait(1));
		CHECK_EQ(in_ring.load(), zero_copy ? 1 : 0);
	}
}

// socket::set_zero_copy switches a live connection between copied and
// zero-copy delivery, e.g. once the application's own handshake trusts the peer.
TEST(set_zero_copy_switches_live_connection)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto c = connect_internal(name, shm_options());
	CHECK(c != nullptr);
	CHECK(c->shared_memory_active());
	std::atomic<int> in_ring{-1};
	waiter           w; // before the socket whose handler uses them
	auto             sock = std::make_shared<datapath::socket>(c);
	sock->on_message([&](const uint8_t* d, size_t n) {
		if (n == 4) {
			in_ring = in_shm_section(d) ? 1 : 0;
			w.hit();
		}
	});
	int expect[] = {0, 1, 0};
	for (int i = 0; i < 3; i++) {
		if (i > 0)
			CHECK(sock->set_zero_copy(i == 1));
		CHECK_EQ(sock->send("ping", 4), error::Success);
		CHECK(w.wait(i + 1));
		CHECK_EQ(in_ring.load(), expect[i]);
	}

	// Without shared memory there is nothing to switch.
	auto c2 = connect_internal(name, socket_options());
	CHECK(c2 != nullptr);
	auto sock2 = std::make_shared<datapath::socket>(c2);
	CHECK(!sock2->set_zero_copy(true));
}

// The peer's process ends without closing anything: the other side is told
// right away through the socket that stays open as the liveness channel.
TEST(peer_process_dies)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);

	char exe[4096] = {};
	CHECK(::readlink("/proc/self/exe", exe, sizeof(exe) - 1) > 0);
	std::string arg_name = name;
	char*       argv[]   = {exe, const_cast<char*>("child"), &arg_name[0], nullptr};
	pid_t       pid      = 0;
	CHECK_EQ(posix_spawn(&pid, exe, nullptr, nullptr, argv, environ), 0);
	CHECK(srv.accepted.wait(1));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	::kill(pid, SIGKILL);
	int status = 0;
	::waitpid(pid, &status, 0);
	auto t0 = std::chrono::steady_clock::now();
	CHECK(srv.closed.wait(1));
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	std::printf("close seen %lld ms after the process ended (reason %d)\n", (long long)ms, srv.close_reason.load());
	CHECK(srv.close_reason.load() != int(error::Success));
}

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "child") == 0)
		return run_child(argv[2]);
	return check::run(argc, argv);
}
