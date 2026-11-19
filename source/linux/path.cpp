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

#include "path.hpp"
#include <cstddef>
#include <cstring>

bool datapath::linux_::make_socket_address(const std::string& path, socket_address& out)
{
	out                  = socket_address{};
	out.addr.sun_family  = AF_UNIX;
	constexpr size_t max = sizeof(out.addr.sun_path);
	if (path.empty())
		return false;
	if (path[0] == '/') {
		if (path.size() >= max)
			return false;
		std::memcpy(out.addr.sun_path, path.data(), path.size());
		out.length          = socklen_t(offsetof(sockaddr_un, sun_path) + path.size() + 1);
		out.filesystem_path = path;
		return true;
	}
	std::string name = "datapath." + path;
	if (name.size() + 1 > max)
		return false;
	std::memcpy(out.addr.sun_path + 1, name.data(), name.size());
	out.length   = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size());
	out.abstract = true;
	return true;
}
