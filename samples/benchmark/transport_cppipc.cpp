/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// cpp-ipc (libipc, https://github.com/mutouyun/cpp-ipc) transport for the
// comparison benchmark.
//
// cpp-ipc has no server, accept or connection: it has named shared-memory
// channels that any process can open as sender or receiver, and a blocking
// recv(). This transport builds a connection the way an application would:
//
// * the server receives on a control channel `<name>.ctl` (ipc::channel, many
//   senders), where a client sends a hello carrying a unique id;
// * each connection is a pair of single-producer single-consumer unicast
//   channels, `<name>.<id>.c2s` and `<name>.<id>.s2c`, the cheapest policy
//   cpp-ipc offers for one peer;
// * one receive thread per connection calls recv() and hands each message to
//   the handler, so echoes are sent from that thread, as with the other
//   transports.
//
// cpp-ipc's own waiting is what "as shipped" measures (--spin 0): recv()
// yields 32 times, then sleeps on a process-shared condition variable, and
// every send() broadcasts that condition. With --spin N the receive thread
// first polls try_recv() for N µs, DataPath's policy, before calling recv().
//
// Messages up to 64 bytes travel inline in the ring (256 slots). Larger ones
// are copied into a per-size shared chunk pool (32 chunks of the size rounded
// up to 1 KiB) and only the chunk id goes through the ring; the receiver reads
// the chunk in place, so a round trip is two copies, as with DataPath's shm.

#include <libipc/ipc.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "bench.hpp"
#include "common/spin.hpp"

#if defined(_WIN32)
#include <process.h>
#define current_pid() _getpid()
#else
#include <unistd.h>
#define current_pid() getpid()
#endif

namespace {
	using pair_chan = ipc::chan<ipc::relat::single, ipc::relat::single, ipc::trans::unicast>;
	using ctl_chan  = ipc::channel;

	constexpr std::uint64_t poll_ms = 100; // blocking recv() timeout, to notice shutdown

	// The harness ends both processes with std::exit(), skipping destructors, and
	// cpp-ipc leaves its shared memory behind unless told to remove it. Remove
	// every channel this process opened when it exits (Linux: /dev/shm files).
	std::mutex               cleanup_lock;
	std::vector<std::string> cleanup_pairs, cleanup_ctls;

	void cleanup_at_exit()
	{
		std::lock_guard<std::mutex> l(cleanup_lock);
		for (auto& n : cleanup_pairs)
			ipc::chan<ipc::relat::single, ipc::relat::single, ipc::trans::unicast>::clear_storage(n.c_str());
		for (auto& n : cleanup_ctls)
			ipc::channel::clear_storage(n.c_str());
	}

	void remove_at_exit(std::vector<std::string>& list, std::string name)
	{
		static bool registered = (std::atexit(cleanup_at_exit), true);
		(void)registered;
		std::lock_guard<std::mutex> l(cleanup_lock);
		list.push_back(std::move(name));
	}

	class ipc_conn : public bench::connection {
		public:
		pair_chan            tx, rx;
		std::mutex           send_lock; // single producer: serialise sends from several threads
		std::mutex           handler_lock;
		bench::message_fn    handler;
		std::thread          thread;
		std::atomic<bool>    stop{false};
		const bench::config& cfg;

		ipc_conn(const bench::config& c, const std::string& tx_name, const std::string& rx_name) : cfg(c)
		{
			remove_at_exit(cleanup_pairs, tx_name);
			remove_at_exit(cleanup_pairs, rx_name);
			// Receiver first, so the peer's sender sees it.
			rx.connect(rx_name.c_str(), ipc::receiver);
			tx.connect(tx_name.c_str(), ipc::sender);
		}

		~ipc_conn() override
		{
			stop = true;
			if (thread.joinable())
				thread.join();
			rx.disconnect();
			tx.disconnect();
		}

		bench::status send(const void* data, size_t size) override
		{
			std::lock_guard<std::mutex> l(send_lock);
			// try_send with a zero timeout fails instead of blocking; send() would
			// wait 100 ms and then overwrite the oldest unread message.
			return tx.try_send(data, size, 0) ? bench::status::ok : bench::status::would_block;
		}

		void on_message(bench::message_fn fn) override
		{
			{
				std::lock_guard<std::mutex> l(handler_lock);
				handler = std::move(fn);
			}
			if (!thread.joinable())
				thread = std::thread([this] { run(); });
		}

		void deliver(ipc::buff_t& b)
		{
			bench::message_fn* h;
			{
				std::lock_guard<std::mutex> l(handler_lock);
				h = &handler;
			}
			if (*h)
				(*h)(static_cast<const uint8_t*>(b.data()), b.size());
		}

		void run()
		{
			bench::tune_io_thread(cfg);
			auto budget = std::chrono::microseconds(cfg.spin);
			while (!stop.load(std::memory_order_relaxed)) {
				datapath::detail::spin_then_block(
					budget,
					[&] {
						ipc::buff_t b = rx.try_recv();
						if (b.empty())
							return false;
						deliver(b);
						return true;
					},
					[&] {
						ipc::buff_t b = rx.recv(poll_ms);
						if (!b.empty())
							deliver(b);
					});
			}
		}
	};

	class ipc_transport : public bench::transport {
		bench::config                        cfg;
		ctl_chan                             ctl;
		std::thread                          accept_thread;
		std::atomic<bool>                    stop{false};
		std::function<void(bench::conn_ptr)> accept_fn;
		std::atomic<unsigned>                counter{0};

		public:
		explicit ipc_transport(const bench::config& c) : cfg(c) {}

		~ipc_transport() override
		{
			stop = true;
			if (accept_thread.joinable())
				accept_thread.join();
		}

		const char* name() const override
		{
			return "cpp-ipc " CPPIPC_VERSION;
		}

		bench::status host(const std::string& path, std::function<void(bench::conn_ptr)> on_accept) override
		{
			accept_fn = std::move(on_accept);
			remove_at_exit(cleanup_ctls, path + ".ctl");
			if (!ctl.connect((path + ".ctl").c_str(), ipc::receiver))
				return bench::status::failed;
			accept_thread = std::thread([this, path] {
				while (!stop.load()) {
					ipc::buff_t b = ctl.recv(poll_ms);
					if (b.empty())
						continue;
					std::string id(static_cast<const char*>(b.data()), b.size());
					auto        conn = std::make_shared<ipc_conn>(cfg, path + "." + id + ".s2c", path + "." + id + ".c2s");
					accept_fn(conn);
				}
			});
			return bench::status::ok;
		}

		bench::status connect(const std::string& path, bench::conn_ptr& out) override
		{
			std::string id   = std::to_string(current_pid()) + "-" + std::to_string(counter++);
			auto        conn = std::make_shared<ipc_conn>(cfg, path + "." + id + ".c2s", path + "." + id + ".s2c");
			ctl_chan    hello((path + ".ctl").c_str(), ipc::sender);
			// No server yet (checked first: a send without receivers logs an error).
			if (hello.recv_count() == 0 || !hello.try_send(id.data(), id.size(), 0))
				return bench::status::not_found;
			// The server connects its receiver when it handles the hello.
			if (!conn->tx.wait_for_recv(1, 5000))
				return bench::status::failed;
			out = conn;
			return bench::status::ok;
		}
	};
} // namespace

std::unique_ptr<bench::transport> bench::make_transport(const config& c)
{
	return std::make_unique<ipc_transport>(c);
}
