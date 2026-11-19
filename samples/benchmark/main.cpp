/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// DataPath latency / throughput benchmark.
//
//   datapath-benchmark local  [client options]       server + client, two processes
//   datapath-benchmark server <name> [--spin us] [--threads n|auto]
//   datapath-benchmark client <name> [options]
//
// Client options:
//   --sizes 8,64,4096,65536                     message sizes in bytes (add 1048576,16777216 for bulk sizes)
//   --count 100000                              round trips per size (large sizes use fewer)
//   --clients 1                                 concurrent ping-pong connections
//   --spin 50                                   I/O thread spin budget in microseconds
//   --threads 1|auto                            I/O threads; auto = one per physical core
//   --load                                      stream 1 MiB messages on an extra connection meanwhile
//   --buffer 65536                              receive buffer per connection
//   --yield / --no-yield                        Sleep(0)/sched_yield after sends and while spinning (default off)
//   --alertable                                 Windows: alertable completion-port waits
//   --runs 1                                    repeat each size N times; report the median of the runs
//                                               and the lowest/highest p99.9 across them
//   --ideal-core off|auto|N                     Windows: one I/O thread per physical core (soft hint);
//                                               local mode gives the server the cores after the client's
//   --stress N                                  local mode: load all cores with N busy threads in a
//                                               separate process (0 = one per logical core)
//   --stress-nice N                             Linux: run the stress process at nice N (default 0)
//   --mmcss / --no-mmcss                        Windows: register I/O threads with MMCSS (default on)
//   --priority 0..3                             I/O thread priority (normal .. time critical, default 2);
//                                               Linux: nice -0/-4/-12/-19 where RLIMIT_NICE allows
//   --pipe-buffer 0                             Windows kernel pipe buffer (0 = same as --buffer)
//
// Latency is the full round trip (send -> echo -> receive), measured with
// QueryPerformanceCounter on Windows and CLOCK_MONOTONIC elsewhere. The next
// ping is sent from inside the receive callback, so no extra thread wake-up
// is included.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "bench.hpp"
#include "clock.hpp"
#include "stats.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <signal.h>
#include <spawn.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using sock_ptr = bench::conn_ptr;
using bench::config;
using bench::status;

namespace {
	constexpr uint8_t tag_ping  = 'P';
	constexpr uint8_t tag_bulk  = 'B';
	constexpr uint8_t tag_stats = 'S';
	constexpr uint8_t tag_quit  = 'Q';


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
#if defined(_WIN32)
		return "windows";
#elif defined(__APPLE__)
		return "macos";
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
		} else if (c.mode == "stress") {
			c.stress      = argc > 2 ? int(std::strtol(argv[2], nullptr, 10)) : 0;
			c.stress_nice = argc > 3 ? int(std::strtol(argv[3], nullptr, 10)) : 0;
			return true;
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
				std::string v = next();
				c.threads     = v == "auto" ? 0 : size_t(std::strtoull(v.c_str(), nullptr, 10));
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
			} else if (a == "--ideal-core") {
				std::string v = next();
				c.ideal_core  = v == "off" ? -2 : v == "auto" ? -1 : int(std::strtol(v.c_str(), nullptr, 10));
			} else if (a == "--stress") {
				c.stress = int(std::strtol(next(), nullptr, 10));
			} else if (a == "--stress-nice") {
				c.stress_nice = int(std::strtol(next(), nullptr, 10));
			} else if (a == "--runs") {
				c.runs = std::max<size_t>(1, size_t(std::strtoull(next(), nullptr, 10)));
			} else if (a == "--mmcss") {
				c.mmcss = true;
			} else if (a == "--no-mmcss") {
				c.mmcss = false;
			} else if (a == "--priority") {
				c.priority = int(std::strtol(next(), nullptr, 10));
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

	// ---------------------------------------------------------------- server

	int run_server(const config& c)
	{
		auto                    tp = bench::make_transport(c);
		std::mutex              lock;
		std::condition_variable cv;
		bool                    quit = false;
		std::vector<sock_ptr>   clients;

		auto ec = tp->host(c.name, [&](sock_ptr s) {
			bench::connection* sock = s.get(); // kept alive by `clients`
			auto               bulk = std::make_shared<uint64_t>(0);
			{
				std::lock_guard<std::mutex> l(lock);
				clients.push_back(s);
			}
			s->on_message([&, sock, bulk](const uint8_t* d, size_t n) {
				if (n == 0)
					return;
				switch (d[0]) {
				case tag_ping:
					while (sock->send(d, n) == status::would_block)
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
		});
		if (ec != status::ok) {
			std::fprintf(stderr, "host failed: %s\n", bench::status_name(ec));
			return 1;
		}

		std::unique_lock<std::mutex> l(lock);
		cv.wait(l, [&] { return quit; });
		std::exit(0); // skip transport teardown: the client is gone
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
			status ec;
			while ((ec = sock->send(payload.data(), payload.size())) == status::would_block)
				std::this_thread::yield();
			if (ec != status::ok)
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

	status connect_retry(bench::transport& tp, const std::string& name, sock_ptr& s)
	{
		status ec = status::not_found;
		for (int i = 0; i < 500; i++) {
			ec = tp.connect(name, s);
			if (ec != status::not_found)
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
                    if (sock->send(chunk.data(), chunk.size()) == status::would_block)
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
			while (sock->send(&q, 1) == status::would_block)
				std::this_thread::yield();
			std::unique_lock<std::mutex> l(lock);
			cv.wait_for(l, std::chrono::seconds(30), [&] { return have_stats; });
			double secs = double(now_ns() - started) / 1e9;
			return double(server_bytes) / secs / double(1ull << 30);
		}
	};

	int run_client(const config& c)
	{
		auto tp = bench::make_transport(c);

		std::vector<std::unique_ptr<pinger>> pingers;
		for (size_t i = 0; i < c.clients; i++) {
			auto p = std::make_unique<pinger>();
			if (auto ec = connect_retry(*tp, c.name, p->sock); ec != status::ok) {
				std::fprintf(stderr, "connect failed: %s\n", bench::status_name(ec));
				return 1;
			}
			pinger* raw = p.get();
			p->sock->on_message([raw](const uint8_t* d, size_t n) { raw->on_message(d, n); });
			pingers.push_back(std::move(p));
		}

		std::unique_ptr<bulk_stream> bulk;
		if (c.load) {
			bulk = std::make_unique<bulk_stream>();
			if (connect_retry(*tp, c.name, bulk->sock) != status::ok)
				return 1;
			bulk->start();
		}

		std::printf("IPC benchmark: transport=%s platform=%s spin=%ldus io_threads=%s clients=%zu buffer=%zu pipe_buffer=%zu yield=%s alertable=%s mmcss=%s priority=%d stress=%s ideal_core=%s load=%s\n",
					tp->name(), platform_name(), c.spin, c.threads ? std::to_string(c.threads).c_str() : "auto", c.clients, c.buffer, c.pipe_buffer, c.yield ? "on" : "off", c.alertable ? "on" : "off", c.mmcss ? "on" : "off", c.priority,
					c.stress < 0 ? "off"
								 : ((c.stress == 0 ? std::string("all cores") : std::to_string(c.stress)) +
									(c.stress_nice ? " nice " + std::to_string(c.stress_nice) : std::string()))
									   .c_str(),
					c.ideal_core == -2 ? "off" : c.ideal_core == -1 ? "auto" : std::to_string(c.ideal_core).c_str(), c.load ? "1MiB stream" : "off");
		std::printf("Full round-trip latency in microseconds.\n");
		if (c.runs > 1)
			std::printf("Each row: median of %zu runs; p99.9 lo/hi = lowest/highest p99.9 of any run.\n", c.runs);
		std::printf("%10s %8s %9s %9s %9s %9s %9s %10s", "size", "count", "min", "p50", "p99", "p99.9", "max", "MiB/s");
		if (c.runs > 1)
			std::printf(" %9s %9s", "p99.9 lo", "p99.9 hi");
		std::printf("\n");

		struct run_result {
			summary s;
			double  mibs;
		};
		auto median = [](std::vector<double> v) {
			std::sort(v.begin(), v.end());
			return v[v.size() / 2];
		};

		// Discarded warm-up: process start-up, page faults and CPU clock ramp-up
		// otherwise land in whichever size is measured first.
		{
			auto deadline = now_ns() + 1000000000; // 1 s
			for (auto& p : pingers) {
				p->payload.assign(64, 0xa5);
				p->payload[0] = tag_ping;
			}
			while (now_ns() < deadline) {
				for (auto& p : pingers) {
					p->samples.clear();
					p->samples.reserve(10000);
					p->warmup   = 0;
					p->count    = 10000;
					p->done     = 0;
					p->finished = false;
					p->send_one();
				}
				for (auto& p : pingers)
					if (!p->wait())
						return 1;
			}
		}

		for (size_t size : c.sizes) {
			size = std::max<size_t>(size, 1);
			// Keep each size to roughly 2 GiB of traffic per client.
			size_t count  = std::max<size_t>(50, std::min(c.count, (size_t(2) << 30) / size));
			size_t warmup = std::max<size_t>(10, count / 20);

			std::vector<run_result> results;
			for (size_t run = 0; run < c.runs; run++) {
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
			results.push_back({summarize(all), mibs});
			}

			auto pick = [&](auto field) {
				std::vector<double> v;
				for (auto& r : results)
					v.push_back(field(r));
				return v;
			};
			auto p999 = pick([](const run_result& r) { return r.s.p999_us; });
			std::printf("%10zu %8zu %9.2f %9.2f %9.2f %9.2f %9.2f %10.1f", size, count * pingers.size(),
						median(pick([](const run_result& r) { return r.s.min_us; })),
						median(pick([](const run_result& r) { return r.s.p50_us; })),
						median(pick([](const run_result& r) { return r.s.p99_us; })), median(p999),
						median(pick([](const run_result& r) { return r.s.max_us; })),
						median(pick([](const run_result& r) { return r.mibs; })));
			if (c.runs > 1)
				std::printf(" %9.2f %9.2f", *std::min_element(p999.begin(), p999.end()),
							*std::max_element(p999.begin(), p999.end()));
			std::printf("\n");
			std::fflush(stdout);
		}

		if (bulk)
			std::printf("Bulk stream under load: %.2f GiB/s (server-confirmed)\n", bulk->finish());

		uint8_t q = tag_quit;
		pingers[0]->sock->send(&q, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		std::fflush(stdout);
		std::exit(0); // skip transport teardown

	}

	// ---------------------------------------------------------------- stress

	// Keep every core busy like ordinary applications would: integer work plus a
	// streaming pass over an 8 MiB buffer per loop to evict other data from the
	// caches. Runs at normal priority until the parent kills the process.
	int run_stress(const config& c)
	{
#ifndef _WIN32
		// The client leaves with std::exit(), skipping child cleanup: die with the
		// parent instead of loading the machine forever (Windows uses a job object).
#ifdef __linux__
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() == 1)
			return 0;
#else
		// No parent-death signal: watch for being re-parented.
		std::thread([parent = getppid()] {
			while (getppid() == parent)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			_exit(0);
		}).detach();
#endif
		// Threads created below inherit the nice value.
		if (c.stress_nice != 0 && setpriority(PRIO_PROCESS, 0, c.stress_nice) != 0)
			std::fprintf(stderr, "stress: setpriority(%d) failed: %s\n", c.stress_nice, std::strerror(errno));
#endif
		unsigned n = c.stress > 0 ? unsigned(c.stress) : std::max(1u, std::thread::hardware_concurrency());
		std::vector<std::thread> ts;
		for (unsigned t = 0; t < n; t++) {
			ts.emplace_back([t] {
				std::vector<uint64_t> buf((8u << 20) / sizeof(uint64_t), t);
				volatile uint64_t     sink = 0;
				uint64_t              x    = 0x9e3779b97f4a7c15ull * (t + 1);
				for (;;) {
					for (int i = 0; i < 100000; i++)
						x ^= (x << 13) ^ (x >> 7) ^ (x << 17);
					for (size_t i = 0; i < buf.size(); i += 8)
						buf[i] += x;
					sink = sink + buf[x % buf.size()];
				}
			});
		}
		for (auto& t : ts)
			t.join();
		return 0;
	}

	// A child process running this executable with `args`; killed on destruction.
	// The client ends the process with std::exit(), so destructors may never run:
	// on Windows every child also joins a kill-on-close job, which ends it when
	// this process exits for any reason.
	struct child_process {
#ifdef _WIN32
		static HANDLE job()
		{
			static HANDLE h = [] {
				HANDLE                               j = CreateJobObjectW(nullptr, nullptr);
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
				info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (j && !SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
					CloseHandle(j);
					j = nullptr;
				}
				return j;
			}();
			return h;
		}

		PROCESS_INFORMATION pi{};
		bool                ok = false;

		explicit child_process(const std::vector<std::string>& args)
		{
			wchar_t exe[MAX_PATH];
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
			for (auto& a : args)
				cmd += L" " + std::wstring(a.begin(), a.end());
			STARTUPINFOW si{};
			si.cb = sizeof(si);
			ok    = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si,
								   &pi)
				 != FALSE;
			if (!ok)
				return;
			if (HANDLE j = job())
				AssignProcessToJobObject(j, pi.hProcess);
			ResumeThread(pi.hThread);
		}
		// Wait up to `ms` for a clean exit, then kill.
		void finish(int ms)
		{
			if (!ok)
				return;
			if (WaitForSingleObject(pi.hProcess, DWORD(ms)) != WAIT_OBJECT_0)
				TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			ok = false;
		}
#else
		pid_t pid = -1;
		bool  ok  = false;

		explicit child_process(const std::vector<std::string>& args)
		{
			std::vector<std::string> all = {"datapath-benchmark"};
			all.insert(all.end(), args.begin(), args.end());
			std::vector<char*> argv;
			for (auto& a : all)
				argv.push_back(&a[0]);
			argv.push_back(nullptr);
			ok = posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, argv.data(), environ) == 0;
		}
		void finish(int ms)
		{
			if (!ok)
				return;
			for (int waited = 0; waited < ms; waited += 50) {
				int status;
				if (waitpid(pid, &status, WNOHANG) == pid) {
					ok = false;
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
			ok = false;
		}
#endif
		~child_process()
		{
			finish(0);
		}
		child_process(const child_process&)            = delete;
		child_process& operator=(const child_process&) = delete;
	};

	// ---------------------------------------------------------------- local

	int run_local(config c)
	{
		c.name = "dp-bench-" + std::to_string(current_pid());
		std::vector<std::string> args = {"server",     c.name,
										 "--spin",     std::to_string(c.spin),
										 "--threads",  c.threads ? std::to_string(c.threads) : "auto",
										 "--buffer",   std::to_string(c.buffer),
										 "--pipe-buffer", std::to_string(c.pipe_buffer),
										 c.yield ? "--yield" : "--no-yield",
										 c.mmcss ? "--mmcss" : "--no-mmcss",
										 "--priority", std::to_string(c.priority)};
		if (c.alertable)
			args.push_back("--alertable");
		// The server's I/O threads take the physical cores after the client's. With
		// --threads auto each process covers every core, so both start at N.
		args.push_back("--ideal-core");
		args.push_back(c.ideal_core == -2   ? "off"
					   : c.ideal_core == -1 ? "auto"
											: std::to_string(c.ideal_core + int(c.threads)));

		std::unique_ptr<child_process> stress;
		if (c.stress >= 0) {
			stress = std::make_unique<child_process>(
				std::vector<std::string>{"stress", std::to_string(c.stress), std::to_string(c.stress_nice)});
			if (!stress->ok) {
				std::fprintf(stderr, "failed to start stress process\n");
				return 1;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500)); // let it ramp up
		}
		child_process server(args);
		if (!server.ok) {
			std::fprintf(stderr, "failed to start server process\n");
			return 1;
		}
		int rc = run_client(c);
		server.finish(5000);
		return rc;
	}
} // namespace

int main(int argc, char** argv)
{
	config c;
	if (!parse(argc, argv, c)) {
		std::fprintf(stderr,
					 "usage: %s local|server <name>|client <name> [--sizes a,b,..] [--count n] [--clients n]\n"
					 "       [--spin us] [--threads n|auto] [--buffer bytes] [--pipe-buffer bytes] [--yield|--no-yield] [--alertable] [--runs n] [--stress n] [--stress-nice n] [--ideal-core off|auto|n] [--mmcss|--no-mmcss] [--priority 0..3] [--load]\n",
					 argc > 0 ? argv[0] : "datapath-benchmark");
		return 2;
	}
	if (c.mode == "server")
		return run_server(c);
	if (c.mode == "client")
		return run_client(c);
	if (c.mode == "stress")
		return run_stress(c);
	return run_local(c);
}

const char* bench::status_name(status s)
{
	switch (s) {
	case status::ok:
		return "ok";
	case status::would_block:
		return "would_block";
	case status::closed:
		return "closed";
	case status::not_found:
		return "not_found";
	case status::failed:
		return "failed";
	}
	return "?";
}

void bench::tune_io_thread(const config& c)
{
#ifdef _WIN32
	static const int levels[] = {THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST,
								 THREAD_PRIORITY_TIME_CRITICAL};
	if (c.priority > 0 && c.priority <= 3)
		SetThreadPriority(GetCurrentThread(), levels[c.priority]);
	if (c.mmcss) {
		static HMODULE avrt = LoadLibraryW(L"avrt.dll");
		using set_t         = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
		using priority_t    = BOOL(WINAPI*)(HANDLE, int);
		if (avrt) {
			auto  set  = reinterpret_cast<set_t>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")));
			auto  prio = reinterpret_cast<priority_t>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadPriority")));
			DWORD task = 0;
			if (set) {
				HANDLE h = set(L"Pro Audio", &task);
				if (h && prio)
					prio(h, 2);
			}
		}
	}
#else
	(void)c;
#endif
}
