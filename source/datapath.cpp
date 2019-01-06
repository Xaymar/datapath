/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 Michael Fabian Dirks <info@xaymar.com>

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

#include "datapath.hpp"
#if defined(_WIN32)
#include "windows/server.hpp"
#include "windows/socket.hpp"
#endif

datapath::error datapath::connect(std::shared_ptr<datapath::isocket>& socket, std::string path)
{
#if defined(_WIN32)
	return datapath::windows::socket::connect(socket, path);
#else
	return datapath::error::Unknown;
#endif
}

datapath::error datapath::host(std::shared_ptr<datapath::iserver>& server, std::string path,
                               datapath::permissions permissions, size_t max_clients)
{
#if defined(_WIN32)
	return datapath::windows::server::host(server, path, permissions, max_clients);
#else
	return datapath::error::Unknown;
#endif
}
