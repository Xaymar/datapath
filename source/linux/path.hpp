/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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
