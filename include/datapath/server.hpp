/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <datapath/export.h>
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

#ifdef _MSC_VER
// C4251 (std::shared_ptr member without dll-interface) is harmless: a C++ API
// already requires the library and program to share one C++ runtime.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
	class DATAPATH_API server {
		std::shared_ptr<detail::listener> _impl;

		public:
		explicit server(std::shared_ptr<detail::listener> impl);
		~server();
		server(const server&)            = delete;
		server& operator=(const server&) = delete;

		void            on_accept(accept_handler handler);
		datapath::error close();
	};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
} // namespace datapath
