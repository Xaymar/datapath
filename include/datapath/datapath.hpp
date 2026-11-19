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
#include <memory>
#include <string>
#include "context.hpp"
#include "error.hpp"
#include "options.hpp"
#include "permissions.hpp"
#include "server.hpp"
#include "socket.hpp"

namespace datapath {
	// Connect to a server. Blocks until the handshake completes or
	// options::connect_timeout elapses. A null context uses the default context.
	datapath::error connect(std::shared_ptr<datapath::socket>& socket, const std::string& path,
							const datapath::options& options = {}, std::shared_ptr<datapath::context> ctx = nullptr);

	// Host a server. Set on_accept before clients connect, or they wait.
	datapath::error host(std::shared_ptr<datapath::server>& server, const std::string& path,
						 const datapath::options& options = {}, std::shared_ptr<datapath::context> ctx = nullptr);
} // namespace datapath
