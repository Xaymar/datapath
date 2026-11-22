/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// options::shared_memory: the version 3 handshake, the data path itself, and
// what happens when the peer's process ends. The rest of the integration suite
// also runs over shared memory (the *.shm tests).

#include <datapath/datapath.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "check.hpp"
#include "common/handshake.hpp"
#include "windows/pipe_connection.hpp"
#include "windows/shm.hpp"
#include "windows/win.hpp"

using datapath::error;
using sock_ptr = std::shared_ptr<datapath::socket>;
using srv_ptr  = std::shared_ptr<datapath::server>;

namespace {
	std::string unique_name()
	{
		static std::atomic<int> n{0};
		return "dp-test-shm-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(n++);
	}

	datapath::options shm_options()
	{
		datapath::options o;
		o.shared_memory = true;
		return o;
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

	// Connect through the context directly to see the connection's internals.
	std::shared_ptr<datapath::windows::pipe_connection> connect_internal(const std::string& name,
																		 const datapath::options& o)
	{
		std::shared_ptr<datapath::detail::connection> conn;
		auto ctx = datapath::context::get_default();
		if (ctx->impl()->connect(conn, name, o) != error::Success)
			return nullptr;
		return std::dynamic_pointer_cast<datapath::windows::pipe_connection>(conn);
	}

	// Echo server whose accepted sockets stay alive until it is destroyed.
	struct echo_server {
		// Declared first, destroyed last: callbacks may still use them while the
		// sockets below are released.
		waiter                closed;
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
				std::lock_guard<std::mutex> l(lock);
				clients.push_back(std::move(s));
				return true;
			});
			return error::Success;
		}
	};

	// Child process for peer_process_dies: connect, send one message, then hang
	// until the parent kills this process.
	int run_child(const char* name)
	{
		sock_ptr c;
		if (datapath::connect(c, name, shm_options()) != error::Success)
			return 2;
		c->send("alive", 5);
		Sleep(INFINITE);
		return 0;
	}
} // namespace

TEST(options_default_off)
{
	datapath::options o;
	CHECK(!o.shared_memory);
	CHECK(o.shared_memory_size > 0);
}

// Both sides opted in: the connection ends up on shared memory and echoes
// messages of every size class, including ones larger than the ring.
TEST(echo_all_sizes)
{
	if (!datapath::windows::shm_channel::supported()) {
		std::printf("SKIP: wait completion packets unavailable\n");
		return;
	}
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto o               = shm_options();
	o.shared_memory_size = size_t(1) << 20; // the server picks the ring size; this is the client's
	auto c               = connect_internal(name, o);
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

// Many small messages in order, sent from the callback side too.
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

// Only one side opted in: the connection works over the pipe.
TEST(client_only_uses_pipe)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name, datapath::options{}), error::Success);
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

TEST(server_only_uses_pipe)
{
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	auto c = connect_internal(name, datapath::options{});
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
	std::string  name = unique_name();
	std::wstring path = L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end());
	std::vector<uint32_t> versions;
	HANDLE                pipes[2];
	for (auto& h : pipes) {
		h = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 2, 4096, 4096, 0, nullptr);
		CHECK(h != INVALID_HANDLE_VALUE);
	}
	std::thread server([&] {
		for (HANDLE h : pipes) {
			if (!ConnectNamedPipe(h, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
				return;
			uint8_t hello[datapath::detail::hello_size] = {};
			DWORD   got                                  = 0;
			ReadFile(h, hello, sizeof(hello), &got, nullptr);
			datapath::detail::hello in{};
			datapath::detail::decode_hello_any(hello, in);
			versions.push_back(in.version);
			datapath::detail::encode_hello(hello, {datapath::detail::protocol_version, 1024});
			WriteFile(h, hello, sizeof(hello), &got, nullptr);
			FlushFileBuffers(h);
			if (in.version != datapath::detail::protocol_version) {
				DisconnectNamedPipe(h);
				continue;
			}
			Sleep(200);
			return;
		}
	});
	auto c = connect_internal(name, shm_options());
	server.join();
	for (HANDLE h : pipes)
		CloseHandle(h);
	CHECK(c != nullptr);
	CHECK(!c->shared_memory_active());
	CHECK_EQ(versions.size(), size_t(2));
	CHECK_EQ(versions[0], datapath::detail::protocol_version_ext);
	CHECK_EQ(versions[1], datapath::detail::protocol_version);
}

// The peer's process ends without closing anything: the other side is told
// within moments, through the pipe that stays open as the liveness channel.
TEST(peer_process_dies)
{
	if (!datapath::windows::shm_channel::supported()) {
		std::printf("SKIP: wait completion packets unavailable\n");
		return;
	}
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);

	wchar_t exe[MAX_PATH];
	CHECK(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
	std::wstring        cmd = L"\"" + std::wstring(exe) + L"\" child " + std::wstring(name.begin(), name.end());
	STARTUPINFOW        si{sizeof(si)};
	PROCESS_INFORMATION pi{};
	CHECK(CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
	CloseHandle(pi.hThread);

	// The child's message arrives (echoed back to a process that ignores it).
	for (int i = 0; i < 500; i++) {
		{
			std::lock_guard<std::mutex> l(srv.lock);
			if (!srv.clients.empty())
				break;
		}
		Sleep(10);
	}
	{
		std::lock_guard<std::mutex> l(srv.lock);
		CHECK_EQ(srv.clients.size(), size_t(1));
	}
	Sleep(100);
	TerminateProcess(pi.hProcess, 9);
	WaitForSingleObject(pi.hProcess, 5000);
	CloseHandle(pi.hProcess);
	auto t0 = std::chrono::steady_clock::now();
	CHECK(srv.closed.wait(1));
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	std::printf("close seen %lld ms after the process ended (reason %d)\n", (long long)ms, srv.close_reason.load());
	CHECK(srv.close_reason.load() != int(error::Success));
}

// The server's side of version 3, seen from a raw client: hello, trailer with
// the shared-memory grant, then the 64-byte offer.
TEST(server_answers_version_3_with_offer)
{
	if (!datapath::windows::shm_channel::supported()) {
		std::printf("SKIP: wait completion packets unavailable\n");
		return;
	}
	auto        name = unique_name();
	echo_server srv;
	CHECK_EQ(srv.start(name), error::Success);
	std::wstring path = L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end());
	HANDLE       h    = INVALID_HANDLE_VALUE;
	for (int i = 0; i < 100 && h == INVALID_HANDLE_VALUE; i++) {
		h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
						SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			WaitNamedPipeW(path.c_str(), 100);
	}
	CHECK(h != INVALID_HANDLE_VALUE);
	uint8_t req[datapath::detail::hello_size + datapath::detail::ext_trailer_size] = {};
	datapath::detail::encode_hello(req, {datapath::detail::protocol_version_ext, 1024});
	datapath::detail::encode_ext_trailer(req + datapath::detail::hello_size, datapath::detail::ext_shared_memory, 0);
	DWORD n = 0;
	CHECK(WriteFile(h, req, sizeof(req), &n, nullptr) && n == sizeof(req));
	uint8_t reply[datapath::detail::hello_size + datapath::detail::ext_trailer_size + datapath::windows::shm_offer_size];
	size_t  got = 0;
	while (got < sizeof(reply)) {
		CHECK(ReadFile(h, reply + got, DWORD(sizeof(reply) - got), &n, nullptr) && n > 0);
		got += n;
	}
	datapath::detail::hello r{};
	CHECK_EQ(datapath::detail::decode_hello_any(reply, r), error::Success);
	CHECK_EQ(r.version, datapath::detail::protocol_version_ext);
	uint32_t cpu = 0, accepted = 0;
	datapath::detail::decode_ext_trailer(reply + datapath::detail::hello_size, cpu, accepted);
	CHECK_EQ(accepted, datapath::detail::ext_shared_memory);
	CHECK(std::memcmp(reply + datapath::detail::hello_size + datapath::detail::ext_trailer_size, "DPSM", 4) == 0);
	CloseHandle(h);
}

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "child") == 0)
		return run_child(argv[2]);
	return check::run(argc, argv);
}
