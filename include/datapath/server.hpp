/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <functional>
#include <memory>
#include "error.hpp"
#include "socket.hpp"

namespace datapath {
	namespace detail {
		class listener;
	}

	// Called on an I/O thread for every client that passed the handshake.
	// Return true to accept. Keep the shared_ptr to keep the connection open.
	using accept_handler = std::function<bool(std::shared_ptr<datapath::socket> client)>;

	class server {
		std::shared_ptr<detail::listener> _impl;

		public:
		explicit server(std::shared_ptr<detail::listener> impl);
		~server();
		server(const server&)            = delete;
		server& operator=(const server&) = delete;

		void            on_accept(accept_handler handler);
		datapath::error close();
	};
} // namespace datapath
