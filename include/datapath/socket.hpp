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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include "error.hpp"

namespace datapath {
	namespace detail {
		class connection;
	}

	struct buffer_view {
		const void* data;
		size_t      size;
	};

	// Called on an I/O thread. `data` is valid only for the duration of the call.
	using message_handler = std::function<void(const uint8_t* data, size_t size)>;
	// Called exactly once, on an I/O thread, when the connection ends.
	using close_handler = std::function<void(datapath::error reason)>;

	// One end of a connection. Destroying the last reference closes the connection.
	class socket {
		std::shared_ptr<detail::connection> _impl;

		public:
		explicit socket(std::shared_ptr<detail::connection> impl);
		~socket();
		socket(const socket&)            = delete;
		socket& operator=(const socket&) = delete;

		// Set the message handler. The first call starts receiving; messages that
		// arrive earlier wait in the kernel.
		void on_message(message_handler handler);
		void on_close(close_handler handler);

		// Queue one message. Thread-safe, never blocks, and is safe to call from
		// handlers. Returns TooLarge, WouldBlock or Closed when not queued.
		datapath::error send(const void* data, size_t size);
		// Queue one message assembled from several parts.
		datapath::error sendv(const buffer_view* parts, size_t count);

		datapath::error close();
		bool            good() const;
		// Negotiated message size limit of this connection.
		uint64_t max_message_size() const;
	};
} // namespace datapath
