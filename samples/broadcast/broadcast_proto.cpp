/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// PROTOTYPE (Linux only): one-to-many broadcast over one shared-memory ring,
// measured against N unicast sends through the library's shared-memory path.
//
// Ring: one writer (the server), any number of readers that map it read-only.
// The writer never waits for a reader: it overwrites the oldest records, and a
// reader that falls a full ring behind notices, skips to the newest data and
// reports how many messages it lost ("lag"). So a slow, stopped or hostile
// client can't stall the server or the other clients, and it can't write into
// the ring at all (F_SEAL_FUTURE_WRITE: no writable mapping can be made after
// the server mapped it).
//
// Section layout (memfd, sealed against shrink/grow/future writes):
//   page 0: header (head, reclaim, seq, ...), each on its own cache line
//   then the data ring, mapped twice back to back so records never wrap
// Record: uint64 position (where the record starts, to catch stale data),
// uint64 index (message number), uint32 length, uint32 0, payload padded to 8.
//
// Reading is a seqlock on positions. Before the writer overwrites bytes that
// were published before, it raises `reclaim` to the first byte that is still
// intact. A reader copies a record out, then checks that `reclaim` has not
// passed the record's start; if it has, the copy may be torn and is thrown
// away (counted as lag). Readers therefore always copy out before delivery.
//
// Waking: a reader spins for its budget, then sets its own `waiting` flag in a
// small control page that only it and the server share, and FUTEX_WAITs on the
// shared `seq` word (allowed on a read-only mapping). After publishing, the
// writer scans the flags and, if any is set, issues one FUTEX_WAKE for all of
// them: one syscall however many clients sleep. A lying client can only make
// the server do that syscall needlessly, or make itself miss wake-ups.

#include <datapath/datapath.hpp>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

namespace {
	int64_t now_ns()
	{
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
	}

	inline void cpu_relax()
	{
#if defined(__x86_64__) || defined(__i386__)
		__builtin_ia32_pause();
#elif defined(__aarch64__)
		asm volatile("yield");
#endif
	}

	constexpr uint32_t magic = 0x42535044; // "DPSB"
	constexpr uint64_t hdr_size = 4096;
	constexpr uint64_t rec_hdr  = 24;

	struct alignas(64) bcast_header {
		uint32_t                          magic;
		uint32_t                          version;
		uint64_t                          capacity;
		uint64_t                          max_message;
		alignas(64) std::atomic<uint64_t> head;    // bytes published
		std::atomic<uint64_t>             last;    // start of the newest record (< head once published)
		alignas(64) std::atomic<uint64_t> reclaim; // bytes below this may be overwritten
		alignas(64) std::atomic<uint32_t> seq;     // futex word, bumped per publish
		alignas(64) std::atomic<uint32_t> closed;  // writer is gone
	};
	static_assert(sizeof(bcast_header) <= hdr_size, "header must fit one page");

	struct alignas(64) client_ctl {
		std::atomic<uint32_t> waiting; // written by the client
	};

	uint64_t align8(uint64_t v)
	{
		return (v + 7) & ~uint64_t(7);
	}

	long futex(std::atomic<uint32_t>* addr, int op, uint32_t val)
	{
		// Not FUTEX_PRIVATE_FLAG: the word is shared between processes.
		return syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), op, val, nullptr, nullptr, 0);
	}

	uint8_t* map_mirror(int fd, uint64_t offset, uint64_t cap, int prot)
	{
		void* base = mmap(nullptr, size_t(2 * cap), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (base == MAP_FAILED)
			return nullptr;
		for (int i = 0; i < 2; i++) {
			void* want = static_cast<uint8_t*>(base) + i * cap;
			if (mmap(want, size_t(cap), prot, MAP_SHARED | MAP_FIXED, fd, off_t(offset)) != want) {
				munmap(base, size_t(2 * cap));
				return nullptr;
			}
		}
		return static_cast<uint8_t*>(base);
	}

	// ------------------------------------------------------------ writer

	class bcast_writer {
		int                      _fd = -1;
		bcast_header*            _hdr = nullptr;
		uint8_t*                 _data = nullptr;
		uint64_t                 _cap;
		uint64_t                 _head = 0, _index = 0;
		std::vector<client_ctl*> _clients;

		public:
		bool create(uint64_t cap, uint64_t max_message)
		{
			_cap = cap;
			_fd  = int(syscall(SYS_memfd_create, "dp-bcast", MFD_CLOEXEC | MFD_ALLOW_SEALING));
			if (_fd < 0 || ftruncate(_fd, off_t(hdr_size + cap)) != 0)
				return false;
			void* h = mmap(nullptr, hdr_size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
			if (h == MAP_FAILED)
				return false;
			_hdr              = new (h) bcast_header{};
			_hdr->magic       = magic;
			_hdr->version     = 1;
			_hdr->capacity    = cap;
			_hdr->max_message = max_message;
			if (!(_data = map_mirror(_fd, hdr_size, cap, PROT_READ | PROT_WRITE)))
				return false;
			// From here on nobody (clients included) can map it writable.
			return fcntl(_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) == 0;
		}
		int fd() const
		{
			return _fd;
		}
		// One control page per client, shared only with that client.
		int add_client()
		{
			int fd = int(syscall(SYS_memfd_create, "dp-bcast-ctl", MFD_CLOEXEC | MFD_ALLOW_SEALING));
			if (fd < 0 || ftruncate(fd, 4096) != 0)
				return -1;
			void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (p == MAP_FAILED)
				return -1;
			fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
			_clients.push_back(new (p) client_ctl{});
			return fd;
		}

		void publish(const void* data, uint32_t len)
		{
			uint64_t need = rec_hdr + align8(len);
			uint64_t end  = _head + need;
			if (end > _cap) {
				uint64_t r = end - _cap;
				if (r > _hdr->reclaim.load(std::memory_order_relaxed)) {
					_hdr->reclaim.store(r, std::memory_order_relaxed);
					// The reclaim mark must be visible before any overwritten byte.
					std::atomic_thread_fence(std::memory_order_release);
				}
			}
			uint8_t* d = _data + (_head & (_cap - 1));
			uint64_t h[3] = {_head, _index++, uint64_t(len)};
			std::memcpy(d, h, rec_hdr);
			std::memcpy(d + rec_hdr, data, len);
			_hdr->last.store(_head, std::memory_order_relaxed);
			_head = end;
			_hdr->head.store(end, std::memory_order_release);
			_hdr->seq.fetch_add(1, std::memory_order_seq_cst);
			wake();
		}
		void wake()
		{
			bool any = false;
			for (auto* c : _clients)
				if (c->waiting.load(std::memory_order_relaxed) && c->waiting.exchange(0, std::memory_order_relaxed))
					any = true;
			if (any)
				futex(&_hdr->seq, FUTEX_WAKE, INT_MAX);
		}
		void close()
		{
			_hdr->closed.store(1, std::memory_order_seq_cst);
			_hdr->seq.fetch_add(1, std::memory_order_seq_cst);
			futex(&_hdr->seq, FUTEX_WAKE, INT_MAX);
		}
	};

	// ------------------------------------------------------------ reader

	class bcast_reader {
		const bcast_header*  _hdr  = nullptr;
		const uint8_t*       _data = nullptr;
		client_ctl*          _ctl  = nullptr;
		uint64_t             _cap = 0, _limit = 0, _tail = 0, _expect = 0;
		bool                 _resync = false;
		std::vector<uint8_t> _scratch;

		public:
		bool attach(int fd, int ctl_fd)
		{
			// Must fail: the ring is read-only for clients.
			void* w = mmap(nullptr, hdr_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (w != MAP_FAILED) {
				std::fprintf(stderr, "client: ring could be mapped writable!\n");
				return false;
			}
			int seals = fcntl(fd, F_GET_SEALS);
			if (seals < 0 || !(seals & F_SEAL_SHRINK) || !(seals & F_SEAL_FUTURE_WRITE))
				return false;
			void* h = mmap(nullptr, hdr_size, PROT_READ, MAP_SHARED, fd, 0);
			if (h == MAP_FAILED)
				return false;
			_hdr = static_cast<const bcast_header*>(h);
			// Read the header once and check it against the file size.
			uint64_t cap = _hdr->capacity, lim = _hdr->max_message;
			off_t    sz  = lseek(fd, 0, SEEK_END);
			if (_hdr->magic != magic || cap < 4096 || (cap & (cap - 1)) || uint64_t(sz) != hdr_size + cap
				|| lim + rec_hdr + 8 > cap / 2)
				return false;
			_cap   = cap;
			_limit = lim;
			if (!(_data = map_mirror(fd, hdr_size, cap, PROT_READ)))
				return false;
			void* c = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ctl_fd, 0);
			if (c == MAP_FAILED)
				return false;
			_ctl  = static_cast<client_ctl*>(c);
			// Neither can the read-only mapping be made writable...
			if (mprotect(const_cast<void*>(h), hdr_size, PROT_READ | PROT_WRITE) == 0) {
				std::fprintf(stderr, "client: ring could be made writable!\n");
				return false;
			}
			// ...and FUTEX_WAIT must work on it (EAGAIN for a stale value, not EFAULT).
			auto* sq = const_cast<std::atomic<uint32_t>*>(&_hdr->seq);
			if (futex(sq, FUTEX_WAIT, sq->load() + 12345) == 0 || errno != EAGAIN) {
				std::fprintf(stderr, "client: futex on read-only mapping failed: %s\n", std::strerror(errno));
				return false;
			}
			_tail = _hdr->head.load(std::memory_order_acquire); // start at "now"
			_resync = true;
			return true;
		}

		bool closed() const
		{
			return _hdr->closed.load(std::memory_order_acquire) != 0;
		}

		// Deliver everything published. on_msg(index, data, len), on_lag(dropped).
		template<typename M, typename L>
		int poll(M&& on_msg, L&& on_lag)
		{
			int n = 0;
			for (;;) {
				uint64_t h = _hdr->head.load(std::memory_order_acquire);
				if (h == _tail)
					return n;
				if (h - _tail > _cap || h < _tail) {
					lapped(h);
					continue;
				}
				const uint8_t* rec = _data + (_tail & (_cap - 1));
				uint64_t       hd[3];
				std::memcpy(hd, rec, rec_hdr);
				uint64_t len  = hd[2] & 0xffffffffu;
				uint64_t step = rec_hdr + align8(len);
				bool     sane = hd[0] == _tail && len <= _limit && step <= h - _tail;
				if (sane) {
					_scratch.resize(size_t(len));
					std::memcpy(_scratch.data(), rec + rec_hdr, size_t(len));
				}
				std::atomic_thread_fence(std::memory_order_acquire);
				if (_hdr->reclaim.load(std::memory_order_relaxed) > _tail) {
					lapped(_hdr->head.load(std::memory_order_acquire)); // torn: writer overtook us
					continue;
				}
				if (!sane)
					return -1; // intact but invalid: the writer broke the protocol
				uint64_t idx = hd[1];
				if (_resync) {
					if (idx > _expect && _expect != 0)
						on_lag(idx - _expect);
					_resync = false;
				} else if (idx != _expect) {
					return -1;
				}
				_expect = idx + 1;
				_tail += step;
				on_msg(idx, _scratch.data(), size_t(len));
				n++;
			}
		}

		// Fell a ring behind: continue at the newest record, which the next
		// poll checks like any other, and report what was skipped there.
		void lapped(uint64_t h)
		{
			_tail = h;
			for (int i = 0; i < 64; i++) {
				h          = _hdr->head.load(std::memory_order_acquire);
				uint64_t l = _hdr->last.load(std::memory_order_acquire);
				if (l < h && h - l <= _cap) {
					_tail = l;
					break;
				}
			}
			_resync = true;
		}

		// Spin, then sleep until something is published.
		void wait(int64_t spin_ns)
		{
			int64_t until = now_ns() + spin_ns;
			do {
				if (_hdr->head.load(std::memory_order_acquire) != _tail)
					return;
				cpu_relax();
			} while (now_ns() < until);
			uint32_t s = _hdr->seq.load(std::memory_order_seq_cst);
			_ctl->waiting.store(1, std::memory_order_seq_cst);
			if (_hdr->head.load(std::memory_order_seq_cst) == _tail && !closed())
				futex(const_cast<std::atomic<uint32_t>*>(&_hdr->seq), FUTEX_WAIT, s);
			_ctl->waiting.store(0, std::memory_order_relaxed);
		}
	};

	// ------------------------------------------------------------ benchmark

	struct config {
		std::string      mode = "broadcast"; // or "unicast"
		int              clients = 4;
		uint32_t         size = 64;
		int              count = 20000, warmup = 1000;
		int              interval_us = 1000;
		int              spin_us = 50;
		uint64_t         ring = uint64_t(4) << 20;
		int              server_cpu = -1;
		std::vector<int> client_cpus;
		int              stress = 0, stress_nice = 0;
		int              stop_client = -1; // SIGSTOP this client after warmup (lag test)
		std::string      out = "/tmp";
	};

	void pin(int cpu)
	{
		if (cpu < 0)
			return;
		cpu_set_t s;
		CPU_ZERO(&s);
		CPU_SET(cpu, &s);
		sched_setaffinity(0, sizeof(s), &s);
	}

	std::vector<int> parse_list(const char* s)
	{
		std::vector<int> v;
		while (*s) {
			v.push_back(std::atoi(s));
			while (*s && *s != ',')
				s++;
			if (*s == ',')
				s++;
		}
		return v;
	}

	constexpr uint64_t end_marker = ~uint64_t(0);

	struct sample {
		uint64_t index;
		int64_t  ns;
	};

	void write_samples(const std::string& path, const std::vector<sample>& v, uint64_t lag)
	{
		FILE* f = std::fopen(path.c_str(), "wb");
		if (!f)
			return;
		std::fwrite(&lag, 8, 1, f);
		std::fwrite(v.data(), sizeof(sample), v.size(), f);
		std::fclose(f);
	}

	// --- broadcast client: argv carries the two descriptors
	int run_bcast_client(int fd, int ctl_fd, const config& c, int id)
	{
		bcast_reader r;
		if (!r.attach(fd, ctl_fd)) {
			std::fprintf(stderr, "client %d: attach failed\n", id);
			return 2;
		}
		std::vector<sample> s;
		s.reserve(size_t(c.count + c.warmup + 16));
		uint64_t lag  = 0;
		bool     done = false;
		while (!done) {
			int rc = r.poll(
				[&](uint64_t, const uint8_t* d, size_t n) {
					int64_t t = now_ns();
					if (n < 16)
						return;
					uint64_t idx;
					int64_t  sent;
					std::memcpy(&idx, d, 8);
					std::memcpy(&sent, d + 8, 8);
					if (idx == end_marker)
						done = true;
					else
						s.push_back({idx, t - sent});
				},
				[&](uint64_t dropped) { lag += dropped; });
			if (rc < 0) {
				std::fprintf(stderr, "client %d: protocol error\n", id);
				return 3;
			}
			if (done || r.closed())
				break;
			r.wait(int64_t(c.spin_us) * 1000);
		}
		write_samples(c.out + "/bc-" + std::to_string(id) + ".bin", s, lag);
		return 0;
	}

	// --- unicast client (library, shared memory)
	int run_unicast_client(const config& c, int id, const std::string& name)
	{
		datapath::context_options co;
		co.spin       = std::chrono::microseconds(c.spin_us);
		auto              ctx = datapath::context::create(co);
		datapath::options o;
		o.shared_memory      = true;
		o.shared_memory_size = size_t(c.ring);
		std::shared_ptr<datapath::socket> sock;
		datapath::error                   ec = datapath::error::Closed;
		for (int i = 0; i < 200 && ec != datapath::error::Success; i++) {
			ec = datapath::connect(sock, name, o, ctx);
			if (ec != datapath::error::Success)
				usleep(10000);
		}
		if (ec != datapath::error::Success) {
			std::fprintf(stderr, "client %d: connect %s\n", id, datapath::to_string(ec));
			return 2;
		}
		std::vector<sample> s;
		s.reserve(size_t(c.count + c.warmup + 16));
		std::mutex              m;
		std::condition_variable cv;
		bool                    done = false;
		sock->on_close([&](datapath::error) {
			std::lock_guard<std::mutex> l(m);
			done = true;
			cv.notify_all();
		});
		sock->on_message([&](const uint8_t* d, size_t n) {
			int64_t t = now_ns();
			if (n < 16)
				return;
			uint64_t idx;
			int64_t  sent;
			std::memcpy(&idx, d, 8);
			std::memcpy(&sent, d + 8, 8);
			if (idx == end_marker) {
				std::lock_guard<std::mutex> l(m);
				done = true;
				cv.notify_all();
			} else {
				s.push_back({idx, t - sent});
			}
		});
		{
			std::unique_lock<std::mutex> l(m);
			cv.wait(l, [&] { return done; });
		}
		sock->close();
		write_samples(c.out + "/bc-" + std::to_string(id) + ".bin", s, 0);
		return 0;
	}

	double pct(std::vector<int64_t>& v, double p)
	{
		if (v.empty())
			return 0;
		size_t k = size_t(p / 100.0 * double(v.size() - 1) + 0.5);
		std::nth_element(v.begin(), v.begin() + k, v.end());
		return double(v[k]) / 1000.0;
	}

	void sleep_until(int64_t t)
	{
		// Sleep most of the way, spin the rest, so the send time is precise.
		int64_t left = t - now_ns();
		if (left > 60000) {
			timespec ts{time_t((left - 50000) / 1000000000), long((left - 50000) % 1000000000)};
			nanosleep(&ts, nullptr);
		}
		while (now_ns() < t)
			cpu_relax();
	}
} // namespace

int main(int argc, char** argv)
{
	config c;
	int    role_client = -1, fd = -1, ctl_fd = -1;
	std::string name;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto        next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--mode")
			c.mode = next();
		else if (a == "--clients")
			c.clients = std::atoi(next());
		else if (a == "--size")
			c.size = uint32_t(std::atoi(next()));
		else if (a == "--count")
			c.count = std::atoi(next());
		else if (a == "--warmup")
			c.warmup = std::atoi(next());
		else if (a == "--interval")
			c.interval_us = std::atoi(next());
		else if (a == "--spin")
			c.spin_us = std::atoi(next());
		else if (a == "--ring")
			c.ring = std::strtoull(next(), nullptr, 0);
		else if (a == "--server-cpu")
			c.server_cpu = std::atoi(next());
		else if (a == "--client-cpus")
			c.client_cpus = parse_list(next());
		else if (a == "--stress")
			c.stress = std::atoi(next());
		else if (a == "--stress-nice")
			c.stress_nice = std::atoi(next());
		else if (a == "--stop-client")
			c.stop_client = std::atoi(next());
		else if (a == "--out")
			c.out = next();
		else if (a == "--as-client")
			role_client = std::atoi(next());
		else if (a == "--fd")
			fd = std::atoi(next());
		else if (a == "--ctl-fd")
			ctl_fd = std::atoi(next());
		else if (a == "--name")
			name = next();
		else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return 1;
		}
	}
	if (c.size < 16)
		c.size = 16;

	if (role_client >= 0) {
		if (!c.client_cpus.empty())
			pin(c.client_cpus[size_t(role_client) % c.client_cpus.size()]);
		return c.mode == "unicast" ? run_unicast_client(c, role_client, name) : run_bcast_client(fd, ctl_fd, c, role_client);
	}

	pin(c.server_cpu);
	std::vector<pid_t> stressors;
	for (int i = 0; i < c.stress; i++) {
		pid_t p = fork();
		if (p == 0) {
			if (c.stress_nice)
				setpriority(PRIO_PROCESS, 0, c.stress_nice);
			volatile uint64_t x = 0;
			for (;;)
				x = x + 1;
		}
		stressors.push_back(p);
	}

	bcast_writer                       w;
	std::shared_ptr<datapath::server>  srv;
	std::shared_ptr<datapath::context> sctx;
	std::mutex                         m;
	std::condition_variable            cv;
	std::vector<std::shared_ptr<datapath::socket>> socks;
	std::vector<int>                   ctl_fds;
	name = "dp-bcast-" + std::to_string(getpid());

	if (c.mode == "broadcast") {
		if (!w.create(c.ring, std::min<uint64_t>(64 * 1024, c.ring / 4))) {
			std::perror("create");
			return 1;
		}
		for (int i = 0; i < c.clients; i++)
			ctl_fds.push_back(w.add_client());
	} else {
		datapath::context_options co;
		co.spin = std::chrono::microseconds(0); // the I/O thread shares the server CPU with the sender
		sctx    = datapath::context::create(co);
		datapath::options o;
		o.shared_memory      = true;
		o.shared_memory_size = size_t(c.ring);
		if (datapath::host(srv, name, o, sctx) != datapath::error::Success) {
			std::fprintf(stderr, "host failed\n");
			return 1;
		}
		srv->on_accept([&](std::shared_ptr<datapath::socket> s) {
			s->on_message([](const uint8_t*, size_t) {});
			std::lock_guard<std::mutex> l(m);
			socks.push_back(s);
			cv.notify_all();
			return true;
		});
	}

	std::vector<pid_t> kids;
	for (int i = 0; i < c.clients; i++) {
		pid_t p = fork();
		if (p == 0) {
			std::vector<std::string> args = {argv[0], "--as-client", std::to_string(i), "--mode", c.mode, "--count",
											 std::to_string(c.count), "--warmup", std::to_string(c.warmup), "--spin",
											 std::to_string(c.spin_us), "--ring", std::to_string(c.ring), "--out",
											 c.out, "--name", name};
			if (!c.client_cpus.empty()) {
				std::string l;
				for (int x : c.client_cpus)
					l += (l.empty() ? "" : ",") + std::to_string(x);
				args.push_back("--client-cpus");
				args.push_back(l);
			}
			if (c.mode == "broadcast") {
				int a = dup(w.fd()), b = dup(ctl_fds[size_t(i)]); // dup clears close-on-exec
				args.push_back("--fd");
				args.push_back(std::to_string(a));
				args.push_back("--ctl-fd");
				args.push_back(std::to_string(b));
			}
			std::vector<char*> av;
			for (auto& s : args)
				av.push_back(&s[0]);
			av.push_back(nullptr);
			execv("/proc/self/exe", av.data());
			_exit(127);
		}
		kids.push_back(p);
	}

	if (c.mode == "unicast") {
		std::unique_lock<std::mutex> l(m);
		if (!cv.wait_for(l, std::chrono::seconds(10), [&] { return socks.size() == size_t(c.clients); })) {
			std::fprintf(stderr, "only %zu clients connected\n", socks.size());
			return 1;
		}
	}
	usleep(200000); // let the readers settle into their wait loops

	std::vector<uint8_t> msg(c.size, 0x5a);
	int64_t              send_cost = 0;
	std::vector<int64_t> costs;
	costs.reserve(size_t(c.count));
	int64_t next = now_ns();
	int     total = c.warmup + c.count;
	for (int i = 0; i < total; i++) {
		next += int64_t(c.interval_us) * 1000;
		sleep_until(next);
		if (i == c.warmup && c.stop_client >= 0 && c.stop_client < c.clients)
			kill(kids[size_t(c.stop_client)], SIGSTOP);
		uint64_t idx = uint64_t(i);
		int64_t  t   = now_ns();
		std::memcpy(msg.data(), &idx, 8);
		std::memcpy(msg.data() + 8, &t, 8);
		if (c.mode == "broadcast") {
			w.publish(msg.data(), c.size);
		} else {
			for (auto& s : socks)
				s->send(msg.data(), msg.size());
		}
		int64_t d = now_ns() - t;
		if (i >= c.warmup)
			costs.push_back(d);
		send_cost += d;
	}
	if (c.stop_client >= 0 && c.stop_client < c.clients)
		kill(kids[size_t(c.stop_client)], SIGCONT);
	uint64_t em = end_marker;
	std::memcpy(msg.data(), &em, 8);
	if (c.mode == "broadcast") {
		w.publish(msg.data(), c.size);
		usleep(100000);
		w.close();
	} else {
		for (auto& s : socks)
			s->send(msg.data(), msg.size());
	}
	for (pid_t p : kids)
		waitpid(p, nullptr, 0);
	for (pid_t p : stressors)
		kill(p, SIGKILL), waitpid(p, nullptr, 0);

	// Gather: every (message, client) latency, and per message the slowest client.
	std::vector<int64_t> all, worst(size_t(total), -1);
	uint64_t             lag = 0, got = 0;
	for (int i = 0; i < c.clients; i++) {
		std::string path = c.out + "/bc-" + std::to_string(i) + ".bin";
		FILE*       f    = std::fopen(path.c_str(), "rb");
		if (!f)
			continue;
		uint64_t l = 0;
		if (std::fread(&l, 8, 1, f) != 1)
			l = 0;
		lag += l;
		sample s;
		while (std::fread(&s, sizeof(s), 1, f) == 1) {
			if (s.index < uint64_t(c.warmup) || s.index >= uint64_t(total))
				continue;
			got++;
			all.push_back(s.ns);
			if (i != c.stop_client)
				worst[size_t(s.index)] = std::max(worst[size_t(s.index)], s.ns);
		}
		std::fclose(f);
		unlink(path.c_str());
	}
	std::vector<int64_t> wv;
	for (size_t i = size_t(c.warmup); i < worst.size(); i++)
		if (worst[i] >= 0)
			wv.push_back(worst[i]);
	std::printf("mode=%s clients=%d size=%u interval=%dus spin=%dus stress=%d received=%" PRIu64 "/%" PRIu64
				" lag=%" PRIu64 "\n",
				c.mode.c_str(), c.clients, c.size, c.interval_us, c.spin_us, c.stress, got,
				uint64_t(c.count) * uint64_t(c.clients), lag);
	std::printf("  per receive  us: p50 %8.2f  p99 %8.2f  p99.9 %8.2f  max %8.2f\n", pct(all, 50), pct(all, 99),
				pct(all, 99.9), pct(all, 100));
	std::printf("  last client  us: p50 %8.2f  p99 %8.2f  p99.9 %8.2f  max %8.2f\n", pct(wv, 50), pct(wv, 99),
				pct(wv, 99.9), pct(wv, 100));
	std::printf("  server send  us: p50 %8.2f  p99 %8.2f  p99.9 %8.2f\n", pct(costs, 50), pct(costs, 99),
				pct(costs, 99.9));
	return 0;
}
