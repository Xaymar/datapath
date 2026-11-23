/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Mojo ping-pong benchmark that mirrors DataPath's samples/benchmark harness
// (https://github.com/Xaymar/datapath, samples/benchmark/main.cpp), so the
// numbers compare directly:
//
//   datapath_mojo_bench --transport=raw|bindings [--bind=main|io]
//                       [--sizes=16,64,...] [--count=100000] [--runs=5]
//                       [--stress=N] [--stress-nice=N]
//
// Two processes: this one (client) launches a child that echoes every message.
// Latency is the full round trip (send -> echo -> receive) per message,
// CLOCK_MONOTONIC (base::TimeTicks is the same clock on Linux; we read it
// directly like DataPath does). The next ping is sent from the receive
// callback. Per size: count = clamp(100000, 50, 2 GiB / size), warm-up
// count/20 (min 10) discarded, every size repeated --runs times, median of the
// runs reported plus the lowest/highest p99.9. A 1 s 64 B warm-up runs first.
// --stress N starts a separate process with N busy threads (0 = one per
// logical CPU) at nice --stress-nice, the same loop as DataPath's.
//
// Transports:
//   raw       Mojo message pipe, C API. A trap on the pipe fires on Mojo's IO
//             thread; the handler reads the message (zero-copy view), records
//             the time and writes the next message right there. This is the
//             lowest-overhead way to use Mojo (no task posting).
//   bigbuffer like bindings, with PingBig(mojo_base.mojom.BigBuffer) =>
//             (BigBuffer): payloads over 64 KiB go through a fresh shared-memory
//             region per message (Chromium's path for large data). Both sides
//             copy, like DataPath: the client builds each BigBuffer from its
//             payload, the server builds the reply from the received bytes.
//   bindings  mojom Remote<Echo>/Receiver<Echo>, Ping(array<uint8>) =>
//             (array<uint8>). --bind=main (default) binds both ends on the
//             main thread, as browsers do (IO thread -> main thread hop on
//             each side); --bind=io binds them on the IO thread.

#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <functional>
#include <thread>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/message_loop/message_pump_type.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "mojo/core/embedder/configuration.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/features.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/datapath_bench/echo.mojom.h"
#include "mojo/public/c/system/message_pipe.h"
#include "mojo/public/c/system/trap.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#include "mojo/public/cpp/system/message_pipe.h"

namespace {

// ------------------------------------------------------------------ harness
// Same clock and statistics as DataPath's clock.hpp / stats.hpp.

int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

struct summary {
  double min_us = 0, p50_us = 0, p99_us = 0, p999_us = 0, max_us = 0;
};

summary summarize(std::vector<int64_t> ns) {
  summary s;
  if (ns.empty())
    return s;
  std::sort(ns.begin(), ns.end());
  auto at = [&](double q) {
    size_t i = size_t(q * double(ns.size() - 1) + 0.5);
    return double(ns[std::min(i, ns.size() - 1)]) / 1000.0;
  };
  s.min_us = double(ns.front()) / 1000.0;
  s.p50_us = at(0.50);
  s.p99_us = at(0.99);
  s.p999_us = at(0.999);
  s.max_us = double(ns.back()) / 1000.0;
  return s;
}

struct config {
  std::string transport = "raw";
  std::string bind = "main";
  std::vector<size_t> sizes = {16,   64,    256,    1024,   4096,
                               16384, 65536, 262144, 1048576};
  size_t count = 100000;
  size_t runs = 1;
  int stress = -1;
  int stress_nice = 0;
  int pin_client = -1, pin_server = -1;  // --pin=C,S like DataPath
};

constexpr uint8_t tag_ping = 'P';
constexpr uint8_t tag_quit = 'Q';
constexpr char kPipeName[] = "bench";

// One measurement batch: `warmup` discarded round trips, then `count` timed.
// Driven from whichever thread delivers replies.
struct batch {
  std::vector<uint8_t> payload;
  std::vector<int64_t> samples;
  size_t warmup = 0, count = 0, done = 0;
  int64_t sent_at = 0;
};

// Which logical CPUs handled the pings, like DataPath's cpu_histogram.
constexpr size_t kMaxCpus = 256;
constexpr uint8_t tag_cpus = 'C';
std::atomic<uint64_t> g_cpu_hits[kMaxCpus];
void cpu_hit() {
  int c = sched_getcpu();
  g_cpu_hits[unsigned(c < 0 ? 0 : c) % kMaxCpus].fetch_add(
      1, std::memory_order_relaxed);
}
void cpu_take(uint64_t* out) {
  for (size_t i = 0; i < kMaxCpus; i++)
    out[i] = g_cpu_hits[i].exchange(0, std::memory_order_relaxed);
}
// "cpu5 97.1% cpu4 2.9%": CPUs with at least 0.1%, busiest first.
std::string cpu_describe(const uint64_t* counts) {
  uint64_t total = 0;
  for (size_t i = 0; i < kMaxCpus; i++)
    total += counts[i];
  std::vector<std::pair<uint64_t, size_t>> v;
  for (size_t i = 0; i < kMaxCpus; i++)
    if (counts[i] && counts[i] * 1000 >= total)
      v.emplace_back(counts[i], i);
  std::sort(v.begin(), v.end(),
            [](auto& a, auto& b) { return a.first > b.first; });
  std::string out;
  char buf[48];
  for (auto& e : v) {
    std::snprintf(buf, sizeof(buf), "%scpu%zu %.1f%%", out.empty() ? "" : " ",
                  e.second, 100.0 * double(e.first) / double(total));
    out += buf;
  }
  return out.empty() ? "-" : out;
}

// ------------------------------------------------------------------ stress
// Verbatim copy of DataPath's run_stress() loop.

int run_stress(const config& c) {
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (getppid() == 1)
    return 0;
  if (c.stress_nice != 0 && setpriority(PRIO_PROCESS, 0, c.stress_nice) != 0)
    std::fprintf(stderr, "stress: setpriority(%d) failed: %s\n", c.stress_nice,
                 std::strerror(errno));
  unsigned n = c.stress > 0 ? unsigned(c.stress)
                            : std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> ts;
  for (unsigned t = 0; t < n; t++) {
    ts.emplace_back([t] {
      std::vector<uint64_t> buf((8u << 20) / sizeof(uint64_t), t);
      volatile uint64_t sink = 0;
      uint64_t x = 0x9e3779b97f4a7c15ull * (t + 1);
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

double process_cpu_seconds(base::ProcessId pid) {
  std::string path = "/proc/" + std::to_string(pid) + "/stat";
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f)
    return -1;
  char buf[1024];
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = 0;
  const char* p = std::strrchr(buf, ')');
  unsigned long long utime = 0, stime = 0;
  if (!p || std::sscanf(p + 2,
                        "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                        &utime, &stime) != 2)
    return -1;
  return double(utime + stime) / double(sysconf(_SC_CLK_TCK));
}

// ------------------------------------------------------------------ raw pipes

// Reads every available message on `pipe`, calling fn(data, size) for each,
// then re-arms `trap`. Runs inside the trap handler (Mojo's IO thread).
template <typename Fn>
void drain_and_rearm(MojoHandle trap, MojoHandle pipe, Fn&& fn) {
  for (;;) {
    for (;;) {
      MojoMessageHandle m;
      MojoResult r = MojoReadMessage(pipe, nullptr, &m);
      if (r != MOJO_RESULT_OK)
        break;
      void* buffer = nullptr;
      uint32_t num_bytes = 0;
      MojoGetMessageData(m, nullptr, &buffer, &num_bytes, nullptr, nullptr);
      fn(static_cast<const uint8_t*>(buffer), size_t(num_bytes));
      MojoDestroyMessage(m);
    }
    uint32_t num_events = 1;
    MojoTrapEvent ev{sizeof(ev)};
    MojoResult r = MojoArmTrap(trap, nullptr, &num_events, &ev);
    if (r == MOJO_RESULT_OK)
      return;
    if (r != MOJO_RESULT_FAILED_PRECONDITION)
      _exit(3);
    if (ev.result != MOJO_RESULT_OK)  // peer closed
      _exit(0);
    // Readable again already: loop.
  }
}

void write_raw(MojoHandle pipe, const void* data, size_t n) {
  MojoResult r = mojo::WriteMessageRaw(mojo::MessagePipeHandle(pipe), data,
                                       uint32_t(n), nullptr, 0,
                                       MOJO_WRITE_MESSAGE_FLAG_NONE);
  if (r != MOJO_RESULT_OK) {
    std::fprintf(stderr, "WriteMessageRaw failed: %d\n", int(r));
    _exit(4);
  }
}

struct raw_server {
  MojoHandle pipe = MOJO_HANDLE_INVALID;
  MojoHandle trap = MOJO_HANDLE_INVALID;

  static void on_event(const MojoTrapEvent* ev) {
    auto* self = reinterpret_cast<raw_server*>(ev->trigger_context);
    if (ev->result != MOJO_RESULT_OK) {
      if (ev->result == MOJO_RESULT_CANCELLED)
        return;
      _exit(0);  // client gone
    }
    self->drain();
  }
  void drain() {
    drain_and_rearm(trap, pipe, [this](const uint8_t* d, size_t n) {
      if (n && d[0] == tag_quit)
        _exit(0);
      if (n == 1 && d[0] == tag_cpus) {
        std::vector<uint8_t> reply(1 + 8 * kMaxCpus);
        reply[0] = tag_cpus;
        uint64_t counts[kMaxCpus];
        cpu_take(counts);
        std::memcpy(reply.data() + 1, counts, sizeof(counts));
        write_raw(pipe, reply.data(), reply.size());
        return;
      }
      cpu_hit();
      write_raw(pipe, d, n);
    });
  }
  void start() {
    MojoCreateTrap(&on_event, nullptr, &trap);
    MojoAddTrigger(trap, pipe, MOJO_HANDLE_SIGNAL_READABLE,
                   MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
                   reinterpret_cast<uintptr_t>(this), nullptr);
    drain();
  }
};

std::atomic<bool> g_quitting{false};

struct raw_client {
  MojoHandle pipe = MOJO_HANDLE_INVALID;
  MojoHandle trap = MOJO_HANDLE_INVALID;
  RAW_PTR_EXCLUSION batch* cur = nullptr;  // hot path, plain pointer
  base::WaitableEvent finished;
  base::WaitableEvent cpus_ready;
  uint64_t peer_cpus[kMaxCpus] = {};

  static void on_event(const MojoTrapEvent* ev) {
    auto* self = reinterpret_cast<raw_client*>(ev->trigger_context);
    if (ev->result != MOJO_RESULT_OK) {
      if (ev->result == MOJO_RESULT_CANCELLED)
        return;
      if (g_quitting)
        return;
      std::fprintf(stderr, "server went away\n");
      _exit(1);
    }
    self->drain();
  }
  void drain() {
    drain_and_rearm(trap, pipe, [this](const uint8_t* d, size_t n) {
      int64_t t = now_ns();
      if (n == 1 + sizeof(peer_cpus) && d[0] == tag_cpus) {
        std::memcpy(peer_cpus, d + 1, sizeof(peer_cpus));
        cpus_ready.Signal();
        return;
      }
      cpu_hit();
      batch& b = *cur;
      if (b.done >= b.warmup)
        b.samples.push_back(t - b.sent_at);
      b.done++;
      if (b.done < b.warmup + b.count)
        send_one();
      else
        finished.Signal();
    });
  }
  void send_one() {
    cur->sent_at = now_ns();
    write_raw(pipe, cur->payload.data(), cur->payload.size());
  }
  void start() {
    MojoCreateTrap(&on_event, nullptr, &trap);
    MojoAddTrigger(trap, pipe, MOJO_HANDLE_SIGNAL_READABLE,
                   MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
                   reinterpret_cast<uintptr_t>(this), nullptr);
    uint32_t num_events = 0;
    MojoArmTrap(trap, nullptr, &num_events, nullptr);
  }
  // Called from the main thread; blocks until the batch is done.
  void run(batch& b) {
    cur = &b;
    finished.Reset();
    send_one();
    finished.Wait();
  }
  bool server_cpus(uint64_t* out) {
    cpus_ready.Reset();
    uint8_t q = tag_cpus;
    write_raw(pipe, &q, 1);
    if (!cpus_ready.TimedWait(base::Seconds(5)))
      return false;
    std::memcpy(out, peer_cpus, sizeof(peer_cpus));
    return true;
  }
  void quit() {
    g_quitting = true;
    uint8_t q = tag_quit;
    write_raw(pipe, &q, 1);
  }
};

// ------------------------------------------------------------------ bindings

class EchoImpl : public datapath_bench::mojom::Echo {
 public:
  explicit EchoImpl(mojo::PendingReceiver<datapath_bench::mojom::Echo> r)
      : receiver_(this, std::move(r)) {
    receiver_.set_disconnect_handler(base::BindOnce([] { _exit(0); }));
  }
  void Cpus(CpusCallback cb) override {
    std::vector<uint64_t> counts(kMaxCpus);
    cpu_take(counts.data());
    std::move(cb).Run(counts);
  }
  void Ping(const std::vector<uint8_t>& data, PingCallback cb) override {
    cpu_hit();
    std::move(cb).Run(data);
  }
  void PingBig(mojo_base::BigBuffer data, PingBigCallback cb) override {
    cpu_hit();
    std::move(cb).Run(mojo_base::BigBuffer(base::span<const uint8_t>(data)));
  }
  void Quit() override { _exit(0); }

 private:
  mojo::Receiver<datapath_bench::mojom::Echo> receiver_;
};

struct bindings_client {
  mojo::Remote<datapath_bench::mojom::Echo> remote;
  RAW_PTR_EXCLUSION batch* cur = nullptr;  // hot path, plain pointer
  base::OnceClosure done_closure;
  bool big = false;

  void send_one() {
    cur->sent_at = now_ns();
    if (big)
      remote->PingBig(mojo_base::BigBuffer(base::span(cur->payload)),
                      base::BindOnce(&bindings_client::on_big_reply,
                                     base::Unretained(this)));
    else
      remote->Ping(cur->payload, base::BindOnce(&bindings_client::on_reply,
                                                base::Unretained(this)));
  }
  void on_big_reply(mojo_base::BigBuffer) { on_reply({}); }
  void on_reply(const std::vector<uint8_t>&) {
    int64_t t = now_ns();
    cpu_hit();
    batch& b = *cur;
    if (b.done >= b.warmup)
      b.samples.push_back(t - b.sent_at);
    b.done++;
    if (b.done < b.warmup + b.count)
      send_one();
    else
      std::move(done_closure).Run();
  }
};

// ------------------------------------------------------------------ main

bool parse(const base::CommandLine& cl, config& c) {
  if (cl.HasSwitch("transport"))
    c.transport = cl.GetSwitchValueASCII("transport");
  if (cl.HasSwitch("bind"))
    c.bind = cl.GetSwitchValueASCII("bind");
  if (cl.HasSwitch("sizes")) {
    c.sizes.clear();
    for (const auto& s :
         base::SplitString(cl.GetSwitchValueASCII("sizes"), ",",
                           base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      size_t v = 0;
      if (!base::StringToSizeT(s, &v))
        return false;
      c.sizes.push_back(v);
    }
  }
  auto num = [&](const char* name, auto& out) {
    if (!cl.HasSwitch(name))
      return true;
    int v = 0;
    if (!base::StringToInt(cl.GetSwitchValueASCII(name), &v))
      return false;
    out = static_cast<std::remove_reference_t<decltype(out)>>(v);
    return true;
  };
  if (!num("count", c.count) || !num("runs", c.runs) ||
      !num("stress", c.stress) || !num("stress-nice", c.stress_nice))
    return false;
  if (cl.HasSwitch("pin")) {
    auto v = base::SplitString(cl.GetSwitchValueASCII("pin"), ",",
                               base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (v.size() != 2 || !base::StringToInt(v[0], &c.pin_client) ||
        !base::StringToInt(v[1], &c.pin_server))
      return false;
  }
  c.runs = std::max<size_t>(1, c.runs);
  return c.transport == "raw" || c.transport == "bindings" ||
         c.transport == "bigbuffer";
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// Mojo core is ipcz-only in current Chromium. On Linux its channel is a Unix
// socket unless the MojoUseEventFd feature (off by default on desktop Linux,
// on for Android) switches the node links to shared memory + eventfd.
const char* backend_name() {
  return base::FeatureList::IsEnabled(mojo::core::kMojoUseEventFd)
             ? "ipcz, MojoUseEventFd (shared memory + eventfd)"
             : "ipcz, socket channel";
}

// Pins every thread of this process (Mojo's IO thread already runs) to `cpu`,
// like DataPath's pin_process() / taskset. Threads created later inherit it.
bool pin_all_threads(int cpu) {
  if (cpu < 0)
    return true;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  bool ok = true;
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d)) {
      int tid = atoi(e->d_name);
      if (tid > 0 && sched_setaffinity(tid, sizeof(set), &set) != 0)
        ok = false;
    }
    closedir(d);
  } else {
    ok = false;
  }
  if (!ok)
    std::fprintf(stderr, "could not pin to cpu%d: %s\n", cpu, std::strerror(errno));
  return ok;
}

int run_child(const config& c, base::Thread& io) {
  if (!pin_all_threads(c.pin_server))
    return 1;
  mojo::IncomingInvitation invitation = mojo::IncomingInvitation::Accept(
      mojo::PlatformChannel::RecoverPassedEndpointFromCommandLine(
          *base::CommandLine::ForCurrentProcess()));
  mojo::ScopedMessagePipeHandle pipe = invitation.ExtractMessagePipe(kPipeName);

  if (c.transport == "raw") {
    auto* s = new raw_server;
    s->pipe = pipe.release().value();
    io.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&raw_server::start, base::Unretained(s)));
    base::RunLoop().Run();  // forever; the server ends with _exit()
    return 0;
  }
  mojo::PendingReceiver<datapath_bench::mojom::Echo> receiver(std::move(pipe));
  if (c.bind == "io") {
    io.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](mojo::PendingReceiver<datapath_bench::mojom::Echo> r) {
                         new EchoImpl(std::move(r));
                       },
                       std::move(receiver)));
  } else {
    new EchoImpl(std::move(receiver));
  }
  base::RunLoop().Run();
  return 0;
}

int run_parent(const config& c, base::Thread& io) {
  // Stress process first, like DataPath (500 ms ramp-up).
  base::Process stress;
  unsigned stress_threads = 0;
  if (c.stress >= 0) {
    stress_threads = c.stress > 0
                         ? unsigned(c.stress)
                         : std::max(1u, std::thread::hardware_concurrency());
    base::CommandLine scl(base::CommandLine::ForCurrentProcess()->GetProgram());
    scl.AppendSwitchASCII("stress-child", base::NumberToString(c.stress));
    scl.AppendSwitchASCII("stress-nice", base::NumberToString(c.stress_nice));
    stress = base::LaunchProcess(scl, base::LaunchOptions());
    if (!stress.IsValid()) {
      std::fprintf(stderr, "failed to start stress process\n");
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  // Server child over a PlatformChannel + invitation.
  mojo::PlatformChannel channel;
  base::LaunchOptions options;
  base::CommandLine ccl(base::CommandLine::ForCurrentProcess()->GetProgram());
  ccl.AppendSwitch("child");
  ccl.AppendSwitchASCII("transport", c.transport);
  ccl.AppendSwitchASCII("bind", c.bind);
  if (c.pin_server >= 0)
    ccl.AppendSwitchASCII("pin", "-1," + base::NumberToString(c.pin_server));
  for (const char* f : {"enable-features", "disable-features"})
    if (base::CommandLine::ForCurrentProcess()->HasSwitch(f))
      ccl.AppendSwitchASCII(f, base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(f));
  channel.PrepareToPassRemoteEndpoint(&options, &ccl);
  mojo::OutgoingInvitation invitation;
  mojo::ScopedMessagePipeHandle pipe = invitation.AttachMessagePipe(kPipeName);
  base::Process server = base::LaunchProcess(ccl, options);
  channel.RemoteProcessLaunchAttempted();
  if (!server.IsValid()) {
    std::fprintf(stderr, "failed to start server process\n");
    return 1;
  }
  mojo::OutgoingInvitation::Send(std::move(invitation), server.Handle(),
                                 channel.TakeLocalEndpoint());

  // Pin after the stress and server processes exist, so they don't inherit it.
  if (!pin_all_threads(c.pin_client))
    return 1;
  double cpu_before = stress.IsValid() ? process_cpu_seconds(stress.Pid()) : -1;
  int64_t wall_before = now_ns();

  // Runs one batch on the configured transport.
  std::function<void(batch&)> run_batch;
  std::function<void()> quit;
  std::function<bool(uint64_t*)> server_cpus;
  raw_client raw;
  bindings_client bind;
  bind.big = c.transport == "bigbuffer";
  if (c.transport == "raw") {
    raw.pipe = pipe.release().value();
    base::WaitableEvent started;
    io.task_runner()->PostTask(FROM_HERE, base::BindOnce(
                                              [](raw_client* r, base::WaitableEvent* e) {
                                                r->start();
                                                e->Signal();
                                              },
                                              &raw, &started));
    started.Wait();
    run_batch = [&](batch& b) { raw.run(b); };
    server_cpus = [&](uint64_t* out) { return raw.server_cpus(out); };
    quit = [&] { raw.quit(); };
  } else if (c.bind == "io") {
    // Remote lives on the IO thread; the main thread waits for each batch.
    base::WaitableEvent started;
    io.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](bindings_client* b, mojo::ScopedMessagePipeHandle p,
               base::WaitableEvent* e) {
              b->remote.Bind(mojo::PendingRemote<datapath_bench::mojom::Echo>(
                  std::move(p), 0));
              e->Signal();
            },
            &bind, std::move(pipe), &started));
    started.Wait();
    run_batch = [&](batch& b) {
      base::WaitableEvent done;
      io.task_runner()->PostTask(
          FROM_HERE, base::BindOnce(
                         [](bindings_client* bc, batch* b, base::WaitableEvent* d) {
                           bc->cur = b;
                           bc->done_closure = base::BindOnce(
                               &base::WaitableEvent::Signal, base::Unretained(d));
                           bc->send_one();
                         },
                         &bind, &b, &done));
      done.Wait();
    };
    server_cpus = [&](uint64_t* out) {
      base::WaitableEvent done;
      io.task_runner()->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](bindings_client* bc, uint64_t* o, base::WaitableEvent* d) {
                bc->remote->Cpus(base::BindOnce(
                    [](uint64_t* o, base::WaitableEvent* d,
                       const std::vector<uint64_t>& v) {
                      std::copy_n(v.begin(), std::min(v.size(), kMaxCpus), o);
                      d->Signal();
                    },
                    o, d));
              },
              &bind, out, &done));
      return done.TimedWait(base::Seconds(5));
    };
    quit = [&] {
      base::WaitableEvent done;
      io.task_runner()->PostTask(FROM_HERE, base::BindOnce(
                                                [](bindings_client* bc,
                                                   base::WaitableEvent* d) {
                                                  bc->remote->Quit();
                                                  d->Signal();
                                                },
                                                &bind, &done));
      done.Wait();
    };
  } else {
    bind.remote.Bind(
        mojo::PendingRemote<datapath_bench::mojom::Echo>(std::move(pipe), 0));
    run_batch = [&](batch& b) {
      base::RunLoop loop;
      bind.cur = &b;
      bind.done_closure = loop.QuitClosure();
      bind.send_one();
      loop.Run();
    };
    server_cpus = [&](uint64_t* out) {
      base::RunLoop loop;
      bind.remote->Cpus(base::BindOnce(
          [](uint64_t* o, base::OnceClosure q, const std::vector<uint64_t>& v) {
            std::copy_n(v.begin(), std::min(v.size(), kMaxCpus), o);
            std::move(q).Run();
          },
          out, loop.QuitClosure()));
      loop.Run();
      return true;
    };
    quit = [&] {
      bind.remote->Quit();
      base::RunLoop loop;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, loop.QuitClosure(), base::Milliseconds(50));
      loop.Run();
    };
  }

  std::printf(
      "IPC benchmark: transport=mojo-%s%s backend=%s platform=linux pin=%s stress=%s\n",
      c.transport.c_str(),
      c.transport != "raw" ? (c.bind == "io" ? "(io thread)" : "(main thread)")
                                : "(trap on io thread)",
      backend_name(),
      c.pin_client < 0 && c.pin_server < 0
          ? "off"
          : (std::to_string(c.pin_client) + "," + std::to_string(c.pin_server)).c_str(),
      c.stress < 0 ? "off"
                   : ((c.stress == 0 ? std::string("all cores")
                                     : std::to_string(c.stress)) +
                      (c.stress_nice ? " nice " + std::to_string(c.stress_nice)
                                     : std::string()))
                         .c_str());
  std::printf("Full round-trip latency in microseconds.\n");
  if (c.runs > 1)
    std::printf(
        "Each row: median of %zu runs; p99.9 lo/hi = lowest/highest p99.9 of "
        "any run.\n",
        c.runs);
  std::printf("%10s %8s %9s %9s %9s %9s %9s %10s", "size", "count", "min",
              "p50", "p99", "p99.9", "max", "MiB/s");
  if (c.runs > 1)
    std::printf(" %9s %9s", "p99.9 lo", "p99.9 hi");
  std::printf("\n");
  std::fflush(stdout);

  // Discarded 1 s warm-up with 64 B messages.
  {
    int64_t deadline = now_ns() + 1000000000;
    batch b;
    b.payload.assign(64, 0xa5);
    b.payload[0] = tag_ping;
    while (now_ns() < deadline) {
      b.samples.clear();
      b.samples.reserve(10000);
      b.warmup = 0;
      b.count = 10000;
      b.done = 0;
      run_batch(b);
    }
  }

  uint64_t cpus[kMaxCpus];
  cpu_take(cpus);  // forget the warm-up
  server_cpus(cpus);

  struct run_result {
    summary s;
    double mibs;
  };
  for (size_t size : c.sizes) {
    size = std::max<size_t>(size, 1);
    size_t count = std::max<size_t>(50, std::min(c.count, (size_t(2) << 30) / size));
    size_t warmup = std::max<size_t>(10, count / 20);
    std::vector<run_result> results;
    for (size_t run = 0; run < c.runs; run++) {
      batch b;
      b.payload.assign(size, 0xa5);
      b.payload[0] = tag_ping;
      b.samples.reserve(count);
      b.warmup = warmup;
      b.count = count;
      b.done = 0;
      int64_t t0 = now_ns();
      run_batch(b);
      double secs = double(now_ns() - t0) / 1e9;
      double mibs = double(size) * double(count + warmup) * 2.0 / secs / (1 << 20);
      results.push_back({summarize(b.samples), mibs});
    }
    auto pick = [&](auto field) {
      std::vector<double> v;
      for (auto& r : results)
        v.push_back(field(r));
      return v;
    };
    auto p999 = pick([](const run_result& r) { return r.s.p999_us; });
    std::printf("%10zu %8zu %9.2f %9.2f %9.2f %9.2f %9.2f %10.1f", size, count,
                median(pick([](const run_result& r) { return r.s.min_us; })),
                median(pick([](const run_result& r) { return r.s.p50_us; })),
                median(pick([](const run_result& r) { return r.s.p99_us; })),
                median(p999),
                median(pick([](const run_result& r) { return r.s.max_us; })),
                median(pick([](const run_result& r) { return r.mibs; })));
    if (c.runs > 1)
      std::printf(" %9.2f %9.2f", *std::min_element(p999.begin(), p999.end()),
                  *std::max_element(p999.begin(), p999.end()));
    std::printf("\n");
    cpu_take(cpus);
    std::string client_where = cpu_describe(cpus);
    std::string server_where = server_cpus(cpus) ? cpu_describe(cpus) : "?";
    std::printf("%10s  client %s | server %s\n", "cpus", client_where.c_str(),
                server_where.c_str());
    std::fflush(stdout);
  }

  if (stress.IsValid()) {
    double cpu_after = process_cpu_seconds(stress.Pid());
    double seconds = double(now_ns() - wall_before) / 1e9;
    if (cpu_before >= 0 && cpu_after >= 0 && seconds > 0) {
      double busy = (cpu_after - cpu_before) / seconds;
      std::printf("stress: %u threads averaged %.1f busy cores over %.1f s\n",
                  stress_threads, busy, seconds);
      if (busy < 0.5 * stress_threads)
        std::fprintf(stderr,
                     "warning: the stress process got under half its CPU "
                     "time; this was not a full-load run\n");
    }
  }
  quit();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  server.WaitForExitWithTimeout(base::Seconds(5), nullptr);
  if (stress.IsValid())
    stress.Terminate(0, false);
  std::fflush(stdout);
  _exit(0);  // skip teardown, like DataPath's client
}

}  // namespace

int main(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
  const base::CommandLine& cl = *base::CommandLine::ForCurrentProcess();
  config c;
  if (!parse(cl, c)) {
    std::fprintf(stderr,
                 "usage: %s --transport=raw|bindings [--bind=main|io] "
                 "[--sizes=a,b,..] [--count=n] [--runs=n] [--stress=n] "
                 "[--stress-nice=n]\n",
                 argv[0]);
    return 2;
  }
  if (cl.HasSwitch("stress-child")) {
    base::StringToInt(cl.GetSwitchValueASCII("stress-child"), &c.stress);
    return run_stress(c);
  }

  base::AtExitManager at_exit;
  // Like content/app/initialize_mojo_core.cc: features first, then core.
  // --enable-features=MojoUseEventFd is passed on to the child.
  base::FeatureList::InitInstance(cl.GetSwitchValueASCII("enable-features"),
                                  cl.GetSwitchValueASCII("disable-features"));
  mojo::core::InitFeatures();
  const bool child = cl.HasSwitch("child");
  mojo::core::Configuration mojo_config;
  mojo_config.is_broker_process = !child;
  mojo::core::Init(mojo_config);

  base::SingleThreadTaskExecutor main_executor;
  base::Thread io("io");
  io.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0));
  mojo::core::ScopedIPCSupport ipc(
      io.task_runner(), mojo::core::ScopedIPCSupport::ShutdownPolicy::FAST);

  return child ? run_child(c, io) : run_parent(c, io);
}
