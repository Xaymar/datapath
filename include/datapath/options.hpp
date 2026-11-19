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
		// Initial receive buffer per connection; also the kernel pipe buffer on Windows.
		size_t receive_buffer = size_t(64) * 1024;
	};
} // namespace datapath
