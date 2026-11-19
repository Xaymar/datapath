/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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
