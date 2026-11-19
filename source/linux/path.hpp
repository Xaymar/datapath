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
#include <sys/socket.h>
#include <sys/un.h>
#include <string>

namespace datapath::linux_ {
	struct socket_address {
		sockaddr_un addr{};
		socklen_t   length   = 0;
		bool        abstract = false;
		std::string filesystem_path; // set when !abstract
	};

	// Paths starting with '/' are filesystem sockets; anything else is a name
	// in the abstract namespace ("\0datapath.<name>"). False if too long/empty.
	bool make_socket_address(const std::string& path, socket_address& out);
} // namespace datapath::linux_
