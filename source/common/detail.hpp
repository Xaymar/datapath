/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Internal interfaces implemented by each platform backend.

#pragma once
#include <datapath/datapath.hpp>
#include <functional>
#include <memory>
#include <string>

namespace datapath { namespace detail {
	class connection {
		public:
		virtual ~connection() = default;

		virtual void            set_message_handler(message_handler handler) = 0;
		virtual void            set_close_handler(close_handler handler)     = 0;
		virtual void            set_drain_handler(drain_handler handler)     = 0;
		virtual void            pause()                                      = 0;
		virtual void            resume()                                     = 0;
		virtual datapath::error send(const buffer_view* parts, size_t count) = 0;
		virtual datapath::error close()                                      = 0;
		// The user dropped their handle: close, wait for running callbacks, and
		// never call a handler again (on_close does not fire).
		virtual void            release()                                    = 0;
		virtual bool            good() const                                 = 0;
		virtual uint64_t        max_message_size() const                     = 0;
	};

	using accept_fn = std::function<bool(std::shared_ptr<connection>)>;

	class listener {
		public:
		virtual ~listener() = default;

		// Connections that completed the handshake before a handler was set are
		// held and offered as soon as one is set.
		virtual void            set_accept_handler(accept_fn handler) = 0;
		virtual datapath::error close()                               = 0;
		// Close, wait for a running accept handler, and never call it again.
		virtual void            release()                             = 0;
	};

	class context_impl {
		public:
		virtual ~context_impl() = default;

		virtual datapath::error connect(std::shared_ptr<connection>& out, const std::string& path,
										const datapath::options& options) = 0;
		virtual datapath::error listen(std::shared_ptr<listener>& out, const std::string& path,
									   const datapath::options& options) = 0;
		// Close every connection and listener, then join the I/O threads.
		virtual void shutdown() = 0;
	};

	std::shared_ptr<context_impl> make_platform_context(const datapath::context_options& options);
}} // namespace datapath::detail
