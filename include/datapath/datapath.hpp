/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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
