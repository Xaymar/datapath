/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// memcpy bandwidth / per-copy latency on this box, to compare with DataPath's
// large-message round trips.
//
//   membw single            per-copy time and GB/s for 64 KiB, 256 KiB, 1 MiB,
//                           hot (same src/dst every time, cache-resident) and
//                           cold (rotating through a buffer set of 512 MiB, far
//                           larger than the 128 MiB L3)
//   membw multi             aggregate memcpy bandwidth with 1,2,4,8,16 threads,
//                           1 MiB copies through per-thread 256 MiB sets
//
// Each thread is pinned to one CPU of the process's affinity mask (physical
// cores first, then their SMT siblings).
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

static double pct(std::vector<int64_t> v, double q) {
  std::sort(v.begin(), v.end());
  size_t i = size_t(q * double(v.size() - 1) + 0.5);
  return double(v[std::min(i, v.size() - 1)]);
}

static uint8_t* alloc(size_t n) {
  void* p = nullptr;
  if (posix_memalign(&p, 4096, n))
    std::abort();
  std::memset(p, 1, n);  // fault in
  return static_cast<uint8_t*>(p);
}

// CPUs of our affinity mask ordered: one per physical core first, then siblings.
static std::vector<int> cpu_order() {
  cpu_set_t set;
  sched_getaffinity(0, sizeof(set), &set);
  std::vector<int> first, second;
  for (int c = 0; c < CPU_SETSIZE; c++) {
    if (!CPU_ISSET(c, &set))
      continue;
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c);
    int lead = c;
    if (FILE* f = std::fopen(path, "r")) {
      if (std::fscanf(f, "%d", &lead) != 1)
        lead = c;
      std::fclose(f);
    }
    (lead == c ? first : second).push_back(c);
  }
  first.insert(first.end(), second.begin(), second.end());
  return first;
}

static void pin(int cpu) {
  cpu_set_t s;
  CPU_ZERO(&s);
  CPU_SET(cpu, &s);
  sched_setaffinity(0, sizeof(s), &s);
}

static void single(int cpu) {
  pin(cpu);
  const size_t set_bytes = size_t(512) << 20;
  uint8_t* src = alloc(set_bytes);
  uint8_t* dst = alloc(set_bytes);
  std::printf("single thread on cpu%d; per-copy time in us, GB/s = size / time (1 GB = 1e9 B)\n", cpu);
  std::printf("%8s %5s %8s %9s %9s %9s %9s %9s\n", "size", "mode", "copies", "p50 us",
              "p99 us", "p99.9 us", "max us", "GB/s p50");
  for (size_t size : {size_t(64) << 10, size_t(256) << 10, size_t(1) << 20}) {
    for (int cold = 0; cold < 2; cold++) {
      size_t slots = set_bytes / size;
      size_t n = std::max<size_t>(20000, 4 * slots);
      if (size >= (size_t(1) << 20))
        n = std::max<size_t>(n, 5000);
      n = std::min<size_t>(n, 50000);
      std::vector<int64_t> t;
      t.reserve(n);
      for (size_t i = 0; i < n + n / 10; i++) {
        size_t si = cold ? (i * 7919) % slots : 0;
        size_t di = cold ? (i * 104729 + 13) % slots : 0;
        int64_t a = now_ns();
        std::memcpy(dst + di * size, src + si * size, size);
        int64_t b = now_ns();
        if (i >= n / 10)
          t.push_back(b - a);
      }
      double p50 = pct(t, 0.5);
      std::printf("%8zu %5s %8zu %9.2f %9.2f %9.2f %9.2f %9.1f\n", size, cold ? "cold" : "hot",
                  t.size(), p50 / 1000, pct(t, 0.99) / 1000, pct(t, 0.999) / 1000,
                  pct(t, 1.0) / 1000, double(size) / p50);
    }
  }
  free(src);
  free(dst);
}

static void multi(const std::vector<int>& cpus) {
  const size_t size = size_t(1) << 20, set_bytes = size_t(256) << 20;
  std::printf("aggregate memcpy bandwidth, 1 MiB copies, cold (per-thread 256 MiB src+dst), 2 s per run\n");
  std::printf("GB/s counts bytes copied (read + write traffic is about 2x that, plus write-allocate)\n");
  std::printf("%8s %-40s %10s %14s\n", "threads", "cpus", "GB/s", "GB/s/thread");
  for (int nt : {1, 2, 4, 8, 16}) {
    if (size_t(nt) > cpus.size())
      break;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false}, stop{false};
    std::vector<uint64_t> bytes(nt, 0);
    std::vector<std::thread> ts;
    std::string where;
    for (int k = 0; k < nt; k++) {
      where += (k ? "," : "") + std::to_string(cpus[k]);
      ts.emplace_back([&, k] {
        pin(cpus[k]);
        uint8_t* src = alloc(set_bytes);
        uint8_t* dst = alloc(set_bytes);
        size_t slots = set_bytes / size;
        ready++;
        while (!go)
          ;
        uint64_t n = 0;
        for (size_t i = 0; !stop.load(std::memory_order_relaxed); i++) {
          std::memcpy(dst + ((i * 104729 + 13) % slots) * size, src + ((i * 7919) % slots) * size, size);
          n += size;
        }
        bytes[k] = n;
        free(src);
        free(dst);
      });
    }
    while (ready < nt)
      ;
    int64_t t0 = now_ns();
    go = true;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop = true;
    for (auto& t : ts)
      t.join();
    double secs = double(now_ns() - t0) / 1e9;
    uint64_t total = 0;
    for (auto b : bytes)
      total += b;
    std::printf("%8d %-40s %10.1f %14.1f\n", nt, where.c_str(), total / secs / 1e9,
                total / secs / 1e9 / nt);
  }
}

int main(int argc, char** argv) {
  std::string m = argc > 1 ? argv[1] : "all";
  auto cpus = cpu_order();
  if (m == "single" || m == "all")
    single(argc > 2 ? std::atoi(argv[2]) : cpus[1 % cpus.size()]);
  if (m == "multi" || m == "all")
    multi(cpus);
}
