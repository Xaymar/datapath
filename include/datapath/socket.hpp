/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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
