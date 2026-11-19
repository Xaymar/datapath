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
