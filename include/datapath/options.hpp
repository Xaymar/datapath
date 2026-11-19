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

#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include "permissions.hpp"

namespace datapath {
	// Settings shared by every socket and server that runs on one context.
	struct context_options {
		// How long an idle I/O thread busy-polls before it blocks in the kernel.
		// Spinning trades CPU for tail latency; 0 disables it.
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
		// Windows only: I/O thread priority, applied before MMCSS and kept if MMCSS
		// registration fails. 0 = normal, 1 = above normal, 2 = highest,
		// 3 = time critical. Normal priority made large-message tails 2x worse.
		int priority = 2;
		// Windows only: steer I/O threads to physical cores (soft hint via the ideal
		// processor, not hard affinity), one thread per core and never two on the
		// hyper-threads of one core. -1 = automatic (default): start at a core
		// derived from the process id, skipping core 0 (which handles most
		// interrupts) when there are more than two cores; measured 64 KiB p99.9
		// 72 -> 52 us and 8 B 12 -> 7 us. -2 = off (OS default). N >= 0 = thread i
		// prefers physical core N + i.
		int ideal_core = -1;
		// Windows only: block in an alertable wait (GetQueuedCompletionStatusEx with
		// fAlertable = TRUE), so the scheduler may run the I/O thread early.
		bool alertable = false;
		// Yield the CPU (Windows: Sleep(0), Linux: sched_yield) after a send has
		// written its data and between busy-poll iterations. Off by default: 5-run
		// medians on Windows showed no gain (slightly higher p50 and p99).
		bool yield = false;
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
		// Kernel pipe buffer on Windows (host() only); 0 = same as receive_buffer.
		size_t pipe_buffer = size_t(1) << 20;
	};
} // namespace datapath
