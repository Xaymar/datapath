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
		// Number of I/O threads.
		size_t io_threads = 1;
		// Yield the CPU (Windows: Sleep(0), Linux: sched_yield) after a send has
		// written its data and between busy-poll iterations, so a peer thread that
		// was just woken on this core runs at once instead of waiting for our
		// time slice to end.
		// Windows only: block in an alertable wait (GetQueuedCompletionStatusEx with
		// fAlertable = TRUE), so the scheduler may run the I/O thread early.
		bool alertable = false;
		// Default on Windows, where it measurably cuts p99 latency; off elsewhere.
#ifdef _WIN32
		bool yield = true;
#else
		bool yield = false;
#endif
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
