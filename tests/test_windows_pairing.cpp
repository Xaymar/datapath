/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// context_options::pair_peer: the version 2 handshake, its fallback to
// version 1 servers, and where the paired I/O threads end up.

#include <datapath/datapath.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "check.hpp"
#include "common/handshake.hpp"
#include "raw_peer.hpp"
#include "windows/win.hpp"

using datapath::error;
using sock_ptr = std::shared_ptr<datapath::socket>;
using srv_ptr  = std::shared_ptr<datapath::server>;

namespace {
	std::string unique_name()
	{
		static std::atomic<int> n{0};
		return "dp-test-pair-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(n++);
	}

	std::shared_ptr<datapath::context> pairing_context(bool pair)
	{
		datapath::context_options co;
		co.io_threads = 1;
		co.pair_peer  = pair;
		return datapath::context::create(co);
	}

	// Logical processors of the core holding `cpu` (group 0), 0 if unknown.
	KAFFINITY core_of(unsigned cpu)
	{
		DWORD len = 0;
		GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
		std::vector<uint8_t> buf(len);
		if (!len
			|| !GetLogicalProcessorInformationEx(
				RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len))
			return 0;
		for (DWORD off = 0; off < len;) {
			auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
			if (e->Processor.GroupMask[0].Group == 0 && (e->Processor.GroupMask[0].Mask & (KAFFINITY(1) << cpu)))
				return e->Processor.GroupMask[0].Mask;
			off += e->Size;
		}
		return 0;
	}

	bool machine_has_smt()
	{
		DWORD_PTR process = 0, system = 0;
		if (GetActiveProcessorGroupCount() != 1 || !GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
			return false;
		for (unsigned cpu = 0; cpu < 64; cpu++) {
			KAFFINITY core = core_of(cpu);
			DWORD_PTR both = DWORD_PTR(core) & process;
			if (both & (both - 1))
				return true;
		}
		return false;
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
		bool wait(int n)
		{
			std::unique_lock<std::mutex> l(lock);
			return cv.wait_for(l, std::chrono::seconds(5), [&] { return count >= n; });
		}
	};

	// Echo one message through a server on `server_ctx` and a client on
	// `client_ctx`, and report the processors that handled it on each side.
	void echo_once(std::shared_ptr<datapath::context> server_ctx, std::shared_ptr<datapath::context> client_ctx,
				   unsigned& server_cpu, unsigned& client_cpu)
	{
		auto    name = unique_name();
		srv_ptr server;
		CHECK_EQ(datapath::host(server, name, {}, server_ctx), error::Success);
		sock_ptr accepted;
		std::atomic<unsigned> scpu{~0u}, ccpu{~0u};
		server->on_accept([&](sock_ptr s) {
			accepted                = s;
			datapath::socket* raw   = s.get();
			s->on_message([raw, &scpu](const uint8_t* d, size_t n) {
				scpu = GetCurrentProcessorNumber();
				raw->send(d, n);
			});
			return true;
		});
		sock_ptr c;
		CHECK_EQ(datapath::connect(c, name, {}, client_ctx), error::Success);
		waiter got;
		c->on_message([&](const uint8_t*, size_t) {
			ccpu = GetCurrentProcessorNumber();
			got.hit();
		});
		// Several round trips, so a thread that was just pinned has moved.
		for (int i = 1; i <= 50; i++) {
			CHECK_EQ(c->send("ping", 4), error::Success);
			CHECK(got.wait(i));
		}
		server_cpu = scpu;
		client_cpu = ccpu;
	}
} // namespace

TEST(options_default_off)
{
	datapath::context_options co;
	CHECK(!co.pair_peer);
}

// Both sides opted in: the two I/O threads run on the two hyper-threads of one
// physical core (when the machine has SMT).
TEST(both_sides_pair_on_one_core)
{
	auto     server_ctx = pairing_context(true);
	auto     client_ctx = pairing_context(true);
	unsigned scpu = 0, ccpu = 0;
	echo_once(server_ctx, client_ctx, scpu, ccpu);
	std::printf("server cpu%u, client cpu%u\n", scpu, ccpu);
	if (!machine_has_smt()) {
		std::printf("SKIP placement check: no SMT available to this process\n");
		return;
	}
	CHECK(scpu != ccpu);
	CHECK(core_of(scpu) != 0);
	CHECK_EQ(core_of(scpu), core_of(ccpu));
}

// Only the client opted in: the server answers version 2 with "no pairing".
TEST(client_only_still_connects)
{
	unsigned scpu = 0, ccpu = 0;
	echo_once(pairing_context(false), pairing_context(true), scpu, ccpu);
}

// Only the server opted in: the client sends version 1 as before.
TEST(server_only_still_connects)
{
	unsigned scpu = 0, ccpu = 0;
	echo_once(pairing_context(true), pairing_context(false), scpu, ccpu);
}

// A server that only knows version 1 answers a version 2 hello with its own
// hello and closes. The client must connect again with version 1.
TEST(falls_back_to_version_1_server)
{
	std::string  name = unique_name();
	std::wstring path = L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end());
	std::vector<uint32_t> versions;
	// Both instances exist before the client starts, so its second attempt never
	// finds the name missing.
	HANDLE pipes[2];
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
			datapath::detail::decode_hello_v2(hello, in);
			versions.push_back(in.version);
			datapath::detail::encode_hello(hello, {datapath::detail::protocol_version, 1024});
			WriteFile(h, hello, sizeof(hello), &got, nullptr);
			FlushFileBuffers(h); // until the client has read it
			if (in.version != datapath::detail::protocol_version) {
				DisconnectNamedPipe(h); // what a version 1 server does after a mismatch
				continue;
			}
			Sleep(200); // keep the accepted connection open while the client checks it
			return;
		}
	});
	auto     ctx = pairing_context(true);
	sock_ptr c;
	CHECK_EQ(datapath::connect(c, name, {}, ctx), error::Success);
	server.join();
	for (HANDLE h : pipes)
		CloseHandle(h);
	CHECK_EQ(versions.size(), size_t(2));
	CHECK_EQ(versions[0], datapath::detail::protocol_version_pair);
	CHECK_EQ(versions[1], datapath::detail::protocol_version);
}

// The server's side of version 2, seen from a raw client: a version 2 hello
// followed by the 8-byte pair trailer.
TEST(server_answers_version_2_with_trailer)
{
	auto    name = unique_name();
	auto    ctx  = pairing_context(true); // destroying a context closes its servers
	srv_ptr server;
	CHECK_EQ(datapath::host(server, name, {}, ctx), error::Success);
	sock_ptr accepted;
	server->on_accept([&](sock_ptr s) {
		accepted = s;
		return true;
	});
	auto peer = raw::connect(name); // retries while the server's instance is busy
	CHECK(peer != nullptr);
	uint8_t hello[datapath::detail::hello_size + datapath::detail::pair_trailer_size] = {};
	datapath::detail::encode_hello(hello, {datapath::detail::protocol_version_pair, 1024});
	CHECK(raw::write(*peer, hello, datapath::detail::hello_size));
	CHECK(raw::read(*peer, hello, sizeof(hello), 5000));
	datapath::detail::hello reply{};
	CHECK_EQ(datapath::detail::decode_hello_v2(hello, reply), error::Success);
	CHECK_EQ(reply.version, datapath::detail::protocol_version_pair);
	int32_t cpu = datapath::detail::decode_pair_trailer(hello + datapath::detail::hello_size);
	std::printf("server offered cpu %d\n", cpu);
	if (machine_has_smt())
		CHECK(cpu >= 0);
}

TEST_MAIN()
