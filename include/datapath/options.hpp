/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include "permissions.hpp"

namespace datapath {
	// Settings shared by every socket and server that runs on one context.
	struct context_options {
		// How long an idle I/O thread busy-polls before it blocks in the kernel.
		// Spinning trades CPU for tail latency; 0 disables it. When spinning I/O
		// threads cover every CPU (e.g. client and server on one machine, both
		// with io_threads = 0), a woken thread waits for a spinner's budget to run
		// out: on Linux 64 B p99.9 measured ~116 us with 50 us vs ~23 us with 0.
		std::chrono::microseconds spin{50};
		// Number of I/O threads. 0 = automatic: one per physical core, so no two
		// I/O threads share the hyper-threads of one core (a busy sibling roughly
		// halves the speed of the other, and SMT only helps a thread that waits).
		// On Windows core 0 is left out when there are more than two cores, the
		// same cores ideal_core's automatic placement uses. Default 1: a single
		// I/O thread had the lowest round-trip latency for one connection.
		size_t io_threads = 1;
		// Windows only: register I/O threads with the Multimedia Class Scheduler
		// Service ("Pro Audio" task, critical priority). Off by default: on a busy
		// system MMCSS throttles the threads once they use their CPU share, which
		// measured as a fixed ~2 ms p99.9 stall (with or without spinning). On an
		// idle system it made no measurable difference.
		bool mmcss = false;
		// I/O thread priority. 0 = normal, 1 = above normal, 2 = highest,
		// 3 = time critical.
		// Windows: thread priority, applied before MMCSS and kept if MMCSS
		// registration fails. Normal priority made large-message tails 2x worse.
		// Linux/Android: nice value lowered by 0/4/12/19 from the one the thread
		// inherited. Unprivileged processes only get as far as RLIMIT_NICE allows
		// (often not at all; CAP_SYS_NICE lifts the limit). Under full CPU load a
		// 12-step gap took 64 B p99.9 from ~8 ms to ~200 us (p50 63 -> 7 us).
		// macOS: quality-of-service class, 1 = user-initiated, 2 and 3 =
		// user-interactive. Needs no privileges.
		int priority = 2;
		// Windows only: steer I/O threads to physical cores (soft hint via the ideal
		// processor, not hard affinity), one thread per core and never two on the
		// hyper-threads of one core. -1 = automatic (default): start at a core
		// derived from the process id, skipping core 0 (which handles most
		// interrupts) when there are more than two cores; measured 64 KiB p99.9
		// 72 -> 52 us and 8 B 12 -> 7 us. Automatic mode also keeps the threads off
		// core 0 with thread affinity (within the process's affinity), because under
		// full CPU load Windows ignored the hint and ran them there. -2 = off (OS
		// default). N >= 0 = thread i prefers physical core N + i.
		int ideal_core = -1;
		// Windows only: block in an alertable wait (GetQueuedCompletionStatusEx with
		// fAlertable = TRUE), so the scheduler may run the I/O thread early.
		bool alertable = false;
		// Yield the CPU (Windows: Sleep(0), Linux/macOS: sched_yield) after a send has
		// written its data and between busy-poll iterations. Off by default: 5-run
		// medians on Windows showed no gain (slightly higher p50 and p99). On Linux
		// it fixes the oversubscribed-spin tail above (116 -> 24 us), but under
		// full CPU load every yield gives a timeslice away (64 B p50 7 us -> 4 ms).
		bool yield = false;
		// Windows only, opt-in: put a client and a server that both set this on the
		// two hyper-threads of one physical core (pinned with thread affinity), so
		// they share its L1/L2 cache and no other thread runs on that core. Under
		// full CPU load on a Ryzen 9 7940HS this measured 16 KiB p99.9 14 us vs
		// 229 us on separate cores; idle it made little difference. Applies only
		// to contexts with io_threads = 1, to the first connection between two such
		// contexts, and only while that connection is open; other connections on
		// the same context then share the pinned core. Needs SMT. A peer without
		// this option, or an older library, connects as usual without pairing.
		bool pair_peer = false;
	};

	// Per-connection / per-server settings.
	struct options {
		// Largest message accepted. The effective limit of a connection is the
		// smaller of both peers' values. send() returns TooLarge above it.
		uint64_t max_message_size = uint64_t(64) << 20;
		// Bytes that may wait in the send queue. send() returns WouldBlock beyond it.
		uint64_t max_queued_bytes = uint64_t(64) << 20;
		// Who may connect (host() only).
		datapath::permissions permissions = datapath::permissions::User;
		// Pending connection slots kept ready (host() only).
		size_t backlog = 8;
		// Maximum simultaneously connected clients, 0 = unlimited (host() only).
		size_t max_clients = 0;
		// Time connect() waits for the server's handshake (connect() only).
		std::chrono::milliseconds connect_timeout{1000};
		// Receive buffer per connection. Frames that fit are delivered straight from
		// it; larger frames use a temporary buffer of their own.
		// Reads adapt their size to the traffic, so a large buffer does not slow
		// down small messages.
		size_t receive_buffer = size_t(256) * 1024;
		// Kernel pipe buffer on Windows (host() only), and kernel socket buffer of
		// each connection on macOS (both ends; the OS default is 8 KiB). 0 = same
		// as receive_buffer. Unused on Linux.
		size_t pipe_buffer = size_t(1) << 20;
	};
} // namespace datapath
