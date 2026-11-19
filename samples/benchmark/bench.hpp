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

// Shared benchmark harness interface. The harness (main.cpp) measures; a
// transport (transport_*.cpp) moves the bytes. Each benchmark executable links
// the harness with exactly one transport, so every IPC stack is measured the
// same way.

#pragma once
#include <datapath/options.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bench {
	struct config {
		std::string         mode, name;
		std::vector<size_t> sizes   = {8, 64, 4096, 65536, 1 << 20, 16 << 20};
		size_t              count   = 100000;
		size_t              clients = 1;
		long                spin    = 50;
		size_t              threads = 1;
		bool                load    = false;
		size_t              buffer      = datapath::options{}.receive_buffer;
		size_t              pipe_buffer = datapath::options{}.pipe_buffer;
		bool                yield       = datapath::context_options{}.yield;
		bool                alertable   = false;
		bool                mmcss       = datapath::context_options{}.mmcss;
		int                 priority    = datapath::context_options{}.priority;
		size_t              runs        = 1;
		int                 stress      = -1; // -1 off, 0 = one thread per logical core
	};

	enum class status { ok, would_block, closed, not_found, failed };

	using message_fn = std::function<void(const uint8_t* data, size_t size)>;

	// One framed, bidirectional connection. send() is thread-safe and never blocks.
	// Messages are delivered on the transport's I/O thread.
	class connection {
		public:
		virtual ~connection()                             = default;
		virtual status send(const void* data, size_t size) = 0;
		// First call starts receiving.
		virtual void on_message(message_fn fn) = 0;
	};
	using conn_ptr = std::shared_ptr<connection>;

	class transport {
		public:
		virtual ~transport()               = default;
		virtual const char* name() const   = 0;
		// Start serving `path`; `on_accept` runs on an I/O thread per client.
		virtual status host(const std::string& path, std::function<void(conn_ptr)> on_accept) = 0;
		virtual status connect(const std::string& path, conn_ptr& out)                         = 0;
	};

	// Implemented once per benchmark executable.
	std::unique_ptr<transport> make_transport(const config& c);

	// Apply the configured Windows scheduling (priority, MMCSS) to the calling
	// thread; transports without their own support call this on their I/O threads.
	void tune_io_thread(const config& c);

	const char* status_name(status s);
} // namespace bench
