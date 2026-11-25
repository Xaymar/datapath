/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Boost.Interprocess transport for the comparison benchmark.
//
// Boost.Interprocess's ready-made message channel is message_queue: a bounded
// queue of fixed-size slots in shared memory, guarded by an interprocess mutex
// and two condition variables (not full / not empty). Like cpp-ipc it has no
// server or connection, so this transport builds one the way an application
// would:
//
// * the server receives on a control queue `<name>.ctl`, where a client sends
//   a hello carrying a unique id;
// * the client creates two queues per connection, `<name>.<id>.c2s` and
//   `<name>.<id>.s2c`, sized for the largest message the run uses, and the
//   server opens them;
// * one receive thread per connection calls receive() and hands each message
//   to the handler.
//
// "As shipped" (--spin 0) blocks in receive() (timed_receive, to notice
// shutdown). With --spin N the receive thread polls try_receive() for N µs
// first, DataPath's policy. send() and receive() each copy the message, so a
// round trip is four copies; message_queue has no zero-copy path.

#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/version.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
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

namespace bip = boost::interprocess;

namespace {
	constexpr auto   poll_timeout = std::chrono::milliseconds(100); // to notice shutdown
	constexpr size_t ctl_size     = 256;

	// Queue sizing: room for the largest message of the run (at least 64 KiB, for
	// the harness's control replies), about 16 MiB per direction.
	// The harness ends both processes with std::exit(), skipping destructors:
	// remove every queue this process created when it exits.
	std::mutex               cleanup_lock;
	std::vector<std::string> cleanup_names;

	void remove_at_exit(std::string name)
	{
		static bool registered = (std::atexit([] {
			std::lock_guard<std::mutex> l(cleanup_lock);
			for (auto& n : cleanup_names)
				bip::message_queue::remove(n.c_str());
		}), true);
		(void)registered;
		std::lock_guard<std::mutex> l(cleanup_lock);
		cleanup_names.push_back(std::move(name));
	}

	size_t max_message(const bench::config& c)
	{
		size_t m = size_t(64) << 10;
		for (size_t s : c.sizes)
			m = std::max(m, s);
		if (c.load)
			m = std::max(m, size_t(1) << 20);
		return m;
	}

	size_t slots_for(size_t max_msg)
	{
		return std::clamp<size_t>((size_t(16) << 20) / max_msg, 4, 256);
	}

	std::chrono::steady_clock::time_point deadline()
	{
		return std::chrono::steady_clock::now() + poll_timeout;
	}

	class bip_conn : public bench::connection {
		public:
		std::string                        tx_name, rx_name;
		bool                               owner; // the client created the queues and removes them
		std::unique_ptr<bip::message_queue> tx, rx;
		std::vector<uint8_t>               buffer;
		std::mutex                         handler_lock;
		bench::message_fn                  handler;
		std::thread                        thread;
		std::atomic<bool>                  stop{false};
		const bench::config&               cfg;

		bip_conn(const bench::config& c, std::string tx_n, std::string rx_n, bool create)
			: tx_name(std::move(tx_n)), rx_name(std::move(rx_n)), owner(create), cfg(c)
		{
			if (create) {
				size_t m = max_message(c);
				size_t n = slots_for(m);
				bip::message_queue::remove(tx_name.c_str());
				bip::message_queue::remove(rx_name.c_str());
				remove_at_exit(tx_name);
				remove_at_exit(rx_name);
				tx = std::make_unique<bip::message_queue>(bip::create_only, tx_name.c_str(), n, m);
				rx = std::make_unique<bip::message_queue>(bip::create_only, rx_name.c_str(), n, m);
			} else {
				tx = std::make_unique<bip::message_queue>(bip::open_only, tx_name.c_str());
				rx = std::make_unique<bip::message_queue>(bip::open_only, rx_name.c_str());
			}
			buffer.resize(rx->get_max_msg_size());
		}

		~bip_conn() override
		{
			stop = true;
			if (thread.joinable())
				thread.join();
			tx.reset();
			rx.reset();
			if (owner) {
				bip::message_queue::remove(tx_name.c_str());
				bip::message_queue::remove(rx_name.c_str());
			}
		}

		bench::status send(const void* data, size_t size) override
		{
			if (size > tx->get_max_msg_size())
				return bench::status::failed;
			// message_queue is thread-safe; try_send fails instead of blocking when full.
			return tx->try_send(data, size, 0) ? bench::status::ok : bench::status::would_block;
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

		void deliver(size_t n)
		{
			bench::message_fn* h;
			{
				std::lock_guard<std::mutex> l(handler_lock);
				h = &handler;
			}
			if (*h)
				(*h)(buffer.data(), n);
		}

		void run()
		{
			bench::tune_io_thread(cfg);
			auto budget = std::chrono::microseconds(cfg.spin);
			while (!stop.load(std::memory_order_relaxed)) {
				datapath::detail::spin_then_block(
					budget,
					[&] {
						bip::message_queue::size_type n;
						unsigned int                  prio;
						if (!rx->try_receive(buffer.data(), buffer.size(), n, prio))
							return false;
						deliver(size_t(n));
						return true;
					},
					[&] {
						bip::message_queue::size_type n;
						unsigned int                  prio;
						if (rx->timed_receive(buffer.data(), buffer.size(), n, prio, deadline()))
							deliver(size_t(n));
					});
			}
		}
	};

	class bip_transport : public bench::transport {
		bench::config                        cfg;
		std::string                          ctl_name;
		std::unique_ptr<bip::message_queue>  ctl;
		std::thread                          accept_thread;
		std::atomic<bool>                    stop{false};
		std::function<void(bench::conn_ptr)> accept_fn;
		std::atomic<unsigned>                counter{0};

		public:
		explicit bip_transport(const bench::config& c) : cfg(c) {}

		~bip_transport() override
		{
			stop = true;
			if (accept_thread.joinable())
				accept_thread.join();
			if (ctl) {
				ctl.reset();
				bip::message_queue::remove(ctl_name.c_str());
			}
		}

		const char* name() const override
		{
			static const std::string n = "Boost.Interprocess " + std::to_string(BOOST_VERSION / 100000) + "." +
										 std::to_string(BOOST_VERSION / 100 % 1000) + " message_queue";
			return n.c_str();
		}

		bench::status host(const std::string& path, std::function<void(bench::conn_ptr)> on_accept) override
		{
			accept_fn = std::move(on_accept);
			ctl_name  = path + ".ctl";
			try {
				bip::message_queue::remove(ctl_name.c_str());
				remove_at_exit(ctl_name);
				ctl = std::make_unique<bip::message_queue>(bip::create_only, ctl_name.c_str(), 64, ctl_size);
			} catch (const bip::interprocess_exception&) {
				return bench::status::failed;
			}
			accept_thread = std::thread([this, path] {
				char buf[ctl_size];
				while (!stop.load()) {
					bip::message_queue::size_type n;
					unsigned int                  prio;
					if (!ctl->timed_receive(buf, sizeof(buf), n, prio, deadline()))
						continue;
					std::string id(buf, size_t(n));
					try {
						accept_fn(std::make_shared<bip_conn>(cfg, path + "." + id + ".s2c", path + "." + id + ".c2s",
															 false));
					} catch (const bip::interprocess_exception&) {
					}
				}
			});
			return bench::status::ok;
		}

		bench::status connect(const std::string& path, bench::conn_ptr& out) override
		{
			std::string id = std::to_string(current_pid()) + "-" + std::to_string(counter++);
			try {
				std::unique_ptr<bip::message_queue> hello;
				try {
					hello = std::make_unique<bip::message_queue>(bip::open_only, (path + ".ctl").c_str());
				} catch (const bip::interprocess_exception&) {
					return bench::status::not_found;
				}
				auto conn = std::make_shared<bip_conn>(cfg, path + "." + id + ".c2s", path + "." + id + ".s2c", true);
				if (!hello->try_send(id.data(), id.size(), 0))
					return bench::status::would_block;
				out = conn;
				return bench::status::ok;
			} catch (const bip::interprocess_exception&) {
				return bench::status::failed;
			}
		}
	};
} // namespace

std::unique_ptr<bench::transport> bench::make_transport(const config& c)
{
	return std::make_unique<bip_transport>(c);
}
