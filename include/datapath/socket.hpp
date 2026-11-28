/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <cstddef>
#include <cstdint>
#include <datapath/export.h>
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

	// Called on an I/O thread once the send queue ran empty after a send()
	// returned WouldBlock: sending makes progress again.
	using drain_handler = std::function<void()>;

	// One end of a connection. Destroying the last reference closes the connection.
#ifdef _MSC_VER
// C4251 (std::shared_ptr member without dll-interface) is harmless: a C++ API
// already requires the library and program to share one C++ runtime.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
	class DATAPATH_API socket {
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
		// Set the handler called when the send queue ran empty after WouldBlock.
		void on_drain(drain_handler handler);

		// Stop delivering messages. Thread-safe; from inside the message handler
		// no further message is delivered after the current one, from another
		// thread at most the message being delivered right now completes. While
		// paused nothing is read from the peer, so its sends back up and return
		// WouldBlock to it: a peer that sends faster than the handler keeps up
		// cannot make this side buffer without bound. The connection closing is
		// noticed (and on_close fires) once receiving resumes, after the messages
		// that came before it, unless this side closes it. Other connections
		// keep running on the same I/O thread.
		void pause();
		// Continue delivering messages, starting with those that were already
		// received. Thread-safe.
		void resume();

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
#ifdef _MSC_VER
#pragma warning(pop)
#endif
} // namespace datapath
