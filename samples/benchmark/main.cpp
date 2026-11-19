/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// DataPath latency / throughput benchmark.
//
//   datapath-benchmark local  [client options]       server + client, two processes
//   datapath-benchmark server <name> [--spin us] [--threads n]
//   datapath-benchmark client <name> [options]
//
// Client options:
//   --sizes 8,64,4096,65536,1048576,16777216   message sizes in bytes
//   --count 100000                              round trips per size (large sizes use fewer)
//   --clients 1                                 concurrent ping-pong connections
//   --spin 50                                   I/O thread spin budget in microseconds
//   --threads 1                                 I/O threads
//   --load                                      stream 1 MiB messages on an extra connection meanwhile
//   --buffer 65536                              receive buffer per connection
//   --yield / --no-yield                        Sleep(0)/sched_yield after sends and while spinning
//                                               (default: on for Windows, off elsewhere)
//   --alertable                                 Windows: alertable completion-port waits
//   --pipe-buffer 0                             Windows kernel pipe buffer (0 = same as --buffer)
//
// Latency is the full round trip (send -> echo -> receive), measured with
// QueryPerformanceCounter on Windows and CLOCK_MONOTONIC elsewhere. The next
// ping is sent from inside the receive callback, so no extra thread wake-up
// is included.

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
#include "clock.hpp"
#include "stats.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using sock_ptr = std::shared_ptr<datapath::socket>;
using datapath::error;

namespace {
	constexpr uint8_t tag_ping  = 'P';
	constexpr uint8_t tag_bulk  = 'B';
	constexpr uint8_t tag_stats = 'S';
	constexpr uint8_t tag_quit  = 'Q';

	struct config {
		std::string           mode, name;
		std::vector<size_t>   sizes = {8, 64, 4096, 65536, 1 << 20, 16 << 20};
		size_t                count = 100000;
		size_t                clients = 1;
		long                  spin    = 50;
		size_t                threads = 1;
		bool                  load    = false;
		size_t                buffer      = datapath::options{}.receive_buffer;
		size_t                pipe_buffer = datapath::options{}.pipe_buffer;
		bool                  yield       = datapath::context_options{}.yield;
		bool                  alertable   = false;
	};

	int current_pid()
	{
#ifdef _WIN32
		return _getpid();
#else
		return int(getpid());
#endif
	}

	const char* platform_name()
	{
#ifdef _WIN32
		return "windows";
#else
		return "linux";
#endif
	}

	bool parse(int argc, char** argv, config& c)
	{
		if (argc < 2)
			return false;
		c.mode = argv[1];
		int i  = 2;
		if (c.mode == "server" || c.mode == "client") {
			if (argc < 3)
				return false;
			c.name = argv[2];
			i      = 3;
		} else if (c.mode != "local") {
			return false;
		}
		for (; i < argc; i++) {
			std::string a    = argv[i];
			auto        next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
			if (a == "--sizes") {
				c.sizes.clear();
				std::string list = next();
				size_t      pos  = 0;
				while (pos < list.size()) {
					size_t end = list.find(',', pos);
					c.sizes.push_back(size_t(std::strtoull(list.substr(pos, end - pos).c_str(), nullptr, 10)));
					if (end == std::string::npos)
						break;
					pos = end + 1;
				}
			} else if (a == "--count") {
				c.count = size_t(std::strtoull(next(), nullptr, 10));
			} else if (a == "--clients") {
				c.clients = size_t(std::strtoull(next(), nullptr, 10));
			} else if (a == "--spin") {
				c.spin = std::strtol(next(), nullptr, 10);
			} else if (a == "--threads") {
				c.threads = size_t(std::strtoull(next(), nullptr, 10));
			} else if (a == "--pipe-buffer") {
				c.pipe_buffer = size_t(std::strtoull(next(), nullptr, 10));
			} else if (a == "--buffer") {
				c.buffer = size_t(std::strtoull(next(), nullptr, 10));
			} else if (a == "--yield") {
				c.yield = true;
			} else if (a == "--no-yield") {
				c.yield = false;
			} else if (a == "--alertable") {
				c.alertable = true;
			} else if (a == "--load") {
				c.load = true;
			} else {
				std::fprintf(stderr, "unknown option %s\n", a.c_str());
				return false;
			}
		}
		if (c.clients == 0)
			c.clients = 1;
		return true;
	}

	std::shared_ptr<datapath::context> make_context(const config& c)
	{
		datapath::context_options o;
		o.spin       = std::chrono::microseconds(c.spin);
		o.io_threads = c.threads;
		o.yield      = c.yield;
		o.alertable  = c.alertable;
		return datapath::context::create(o);
	}

	// ---------------------------------------------------------------- server

	int run_server(const config& c)
	{
		auto                              ctx = make_context(c);
		std::shared_ptr<datapath::server> srv;
		datapath::options                 opts;
		opts.max_message_size = uint64_t(1) << 30;
		opts.max_queued_bytes = uint64_t(256) << 20;
		opts.receive_buffer   = c.buffer;
		opts.pipe_buffer      = c.pipe_buffer;
		if (auto ec = datapath::host(srv, c.name, opts, ctx); ec != error::Success) {
			std::fprintf(stderr, "host failed: %s\n", datapath::to_string(ec));
			return 1;
		}
		std::mutex              lock;
		std::condition_variable cv;
		bool                    quit = false;
		std::vector<sock_ptr>   clients;

		srv->on_accept([&](sock_ptr s) {
			std::weak_ptr<datapath::socket> weak  = s;
			auto                            bulk  = std::make_shared<uint64_t>(0);
			s->on_message([&, weak, bulk](const uint8_t* d, size_t n) {
				if (n == 0)
					return;
				auto sock = weak.lock();
				if (!sock)
					return;
				switch (d[0]) {
				case tag_ping:
					while (sock->send(d, n) == error::WouldBlock)
						std::this_thread::yield();
					break;
				case tag_bulk:
					*bulk += n;
					break;
				case tag_stats: {
					uint8_t reply[9] = {tag_stats};
					std::memcpy(reply + 1, bulk.get(), 8);
					*bulk = 0;
					sock->send(reply, sizeof(reply));
					break;
				}
				case tag_quit: {
					std::lock_guard<std::mutex> l(lock);
					quit = true;
					cv.notify_all();
					break;
				}
				}
			});
			std::lock_guard<std::mutex> l(lock);
			clients.push_back(s);
			return true;
		});

		std::unique_lock<std::mutex> l(lock);
		cv.wait(l, [&] { return quit; });
		clients.clear();
		return 0;
	}

	// ---------------------------------------------------------------- client

	struct pinger {
		sock_ptr                sock;
		std::vector<uint8_t>    payload;
		std::vector<int64_t>    samples;
		size_t                  warmup = 0, count = 0, done = 0;
		int64_t                 sent_at = 0;
		std::mutex              lock;
		std::condition_variable cv;
		bool                    finished = false;
		bool                    failed   = false;

		void send_one()
		{
			sent_at = now_ns();
			error ec;
			while ((ec = sock->send(payload.data(), payload.size())) == error::WouldBlock)
				std::this_thread::yield();
			if (ec != error::Success)
				finish(true);
		}

		void finish(bool fail)
		{
			std::lock_guard<std::mutex> l(lock);
			failed   = fail;
			finished = true;
			cv.notify_all();
		}

		void on_message(const uint8_t*, size_t)
		{
			int64_t t = now_ns();
			if (done >= warmup)
				samples.push_back(t - sent_at);
			done++;
			if (done < warmup + count)
				send_one();
			else
				finish(false);
		}

		bool wait()
		{
			std::unique_lock<std::mutex> l(lock);
			cv.wait(l, [&] { return finished; });
			return !failed;
		}
	};

	error connect_retry(sock_ptr& s, const std::string& name, const datapath::options& o,
						std::shared_ptr<datapath::context> ctx)
	{
		error ec = error::NotFound;
		for (int i = 0; i < 500; i++) {
			ec = datapath::connect(s, name, o, ctx);
			if (ec != error::NotFound)
				return ec;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return ec;
	}

	struct bulk_stream {
		sock_ptr              sock;
		std::thread           thread;
		std::atomic<bool>     stop{false};
		int64_t               started = 0;
		std::mutex            lock;
		std::condition_variable cv;
		bool                  have_stats = false;
		uint64_t              server_bytes = 0;

		void start()
		{
			sock->on_message([this](const uint8_t* d, size_t n) {
				if (n == 9 && d[0] == tag_stats) {
					std::lock_guard<std::mutex> l(lock);
					std::memcpy(&server_bytes, d + 1, 8);
					have_stats = true;
					cv.notify_all();
				}
			});
			started = now_ns();
			thread  = std::thread([this] {
                std::vector<uint8_t> chunk(1 << 20, 0x5a);
                chunk[0] = tag_bulk;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (sock->send(chunk.data(), chunk.size()) == error::WouldBlock)
                        std::this_thread::yield();
                }
			});
		}

		// Stop and return the server-confirmed GiB/s.
		double finish()
		{
			stop = true;
			thread.join();
			uint8_t q = tag_stats;
			while (sock->send(&q, 1) == error::WouldBlock)
				std::this_thread::yield();
			std::unique_lock<std::mutex> l(lock);
			cv.wait_for(l, std::chrono::seconds(30), [&] { return have_stats; });
			double secs = double(now_ns() - started) / 1e9;
			return double(server_bytes) / secs / double(1ull << 30);
		}
	};

	int run_client(const config& c)
	{
		auto              ctx = make_context(c);
		datapath::options opts;
		opts.max_message_size = uint64_t(1) << 30;
		opts.max_queued_bytes = uint64_t(256) << 20;
		opts.connect_timeout  = std::chrono::milliseconds(5000);
		opts.receive_buffer   = c.buffer;

		std::vector<std::unique_ptr<pinger>> pingers;
		for (size_t i = 0; i < c.clients; i++) {
			auto p = std::make_unique<pinger>();
			if (auto ec = connect_retry(p->sock, c.name, opts, ctx); ec != error::Success) {
				std::fprintf(stderr, "connect failed: %s\n", datapath::to_string(ec));
				return 1;
			}
			pinger* raw = p.get();
			p->sock->on_message([raw](const uint8_t* d, size_t n) { raw->on_message(d, n); });
			pingers.push_back(std::move(p));
		}

		std::unique_ptr<bulk_stream> bulk;
		if (c.load) {
			bulk = std::make_unique<bulk_stream>();
			if (connect_retry(bulk->sock, c.name, opts, ctx) != error::Success)
				return 1;
			bulk->start();
		}

		std::printf("DataPath benchmark: platform=%s spin=%ldus io_threads=%zu clients=%zu buffer=%zu pipe_buffer=%zu yield=%s alertable=%s load=%s\n",
					platform_name(), c.spin, c.threads, c.clients, c.buffer, c.pipe_buffer, c.yield ? "on" : "off", c.alertable ? "on" : "off", c.load ? "1MiB stream" : "off");
		std::printf("Full round-trip latency in microseconds.\n");
		std::printf("%10s %8s %9s %9s %9s %9s %9s %10s\n", "size", "count", "min", "p50", "p99", "p99.9", "max",
					"MiB/s");

		for (size_t size : c.sizes) {
			size = std::max<size_t>(size, 1);
			// Keep each size to roughly 2 GiB of traffic per client.
			size_t count  = std::max<size_t>(50, std::min(c.count, (size_t(2) << 30) / size));
			size_t warmup = std::max<size_t>(10, count / 20);

			int64_t t0 = now_ns();
			for (auto& p : pingers) {
				p->payload.assign(size, 0xa5);
				p->payload[0] = tag_ping;
				p->samples.clear();
				p->samples.reserve(count);
				p->warmup   = warmup;
				p->count    = count;
				p->done     = 0;
				p->finished = false;
				p->send_one();
			}
			std::vector<int64_t> all;
			for (auto& p : pingers) {
				if (!p->wait()) {
					std::fprintf(stderr, "connection failed during size %zu\n", size);
					return 1;
				}
				all.insert(all.end(), p->samples.begin(), p->samples.end());
			}
			double secs = double(now_ns() - t0) / 1e9;
			double mibs = double(size) * double((count + warmup) * pingers.size()) * 2.0 / secs / (1 << 20);
			auto   s    = summarize(all);
			std::printf("%10zu %8zu %9.2f %9.2f %9.2f %9.2f %9.2f %10.1f\n", size, count * pingers.size(), s.min_us,
						s.p50_us, s.p99_us, s.p999_us, s.max_us, mibs);
			std::fflush(stdout);
		}

		if (bulk)
			std::printf("Bulk stream under load: %.2f GiB/s (server-confirmed)\n", bulk->finish());

		uint8_t q = tag_quit;
		pingers[0]->sock->send(&q, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		return 0;
	}

	// ---------------------------------------------------------------- local

	int run_local(int argc, char** argv, config c)
	{
		c.name = "dp-bench-" + std::to_string(current_pid());
		std::string spin    = std::to_string(c.spin);
		std::string threads = std::to_string(c.threads);
		std::string buffer  = std::to_string(c.buffer);
		std::string pipebuf = std::to_string(c.pipe_buffer);
#ifdef _WIN32
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring cmd = L"\"" + std::wstring(exe) + L"\" server " + std::wstring(c.name.begin(), c.name.end())
						   + L" --spin " + std::wstring(spin.begin(), spin.end()) + L" --threads "
						   + std::wstring(threads.begin(), threads.end()) + L" --buffer "
						   + std::wstring(buffer.begin(), buffer.end()) + L" --pipe-buffer "
						   + std::wstring(pipebuf.begin(), pipebuf.end()) + (c.yield ? L" --yield" : L" --no-yield")
						   + (c.alertable ? L" --alertable" : L"");
		STARTUPINFOW        si{};
		PROCESS_INFORMATION pi{};
		si.cb = sizeof(si);
		if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
			std::fprintf(stderr, "failed to start server process\n");
			return 1;
		}
		int rc = run_client(c);
		if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0)
			TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return rc;
#else
		(void)argc;
		(void)argv;
		std::vector<std::string> args = {"datapath-benchmark", "server", c.name, "--spin",  spin,
										  "--threads",          threads, "--buffer", buffer, "--pipe-buffer", pipebuf};
		args.push_back(c.yield ? "--yield" : "--no-yield");
		if (c.alertable)
			args.push_back("--alertable");
		std::vector<char*>       cargv;
		for (auto& a : args)
			cargv.push_back(&a[0]);
		cargv.push_back(nullptr);
		pid_t pid;
		if (posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, cargv.data(), environ) != 0) {
			std::fprintf(stderr, "failed to start server process\n");
			return 1;
		}
		int rc = run_client(c);
		for (int i = 0; i < 100; i++) {
			int status;
			if (waitpid(pid, &status, WNOHANG) == pid)
				return rc;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
		return rc;
#endif
	}
} // namespace

int main(int argc, char** argv)
{
	config c;
	if (!parse(argc, argv, c)) {
		std::fprintf(stderr,
					 "usage: %s local|server <name>|client <name> [--sizes a,b,..] [--count n] [--clients n]\n"
					 "       [--spin us] [--threads n] [--buffer bytes] [--pipe-buffer bytes] [--yield|--no-yield] [--alertable] [--load]\n",
					 argc > 0 ? argv[0] : "datapath-benchmark");
		return 2;
	}
	if (c.mode == "server")
		return run_server(c);
	if (c.mode == "client")
		return run_client(c);
	return run_local(argc, argv, c);
}
