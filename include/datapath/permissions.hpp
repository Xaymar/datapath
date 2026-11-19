/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <cstdint>

namespace datapath {
	// Who may connect to a hosted server.
	enum class permissions : uint8_t {
		// Only the user that created the server.
		User,
		// The creating user and the local user group (Windows: BUILTIN\Users, Linux: creator's primary group).
		Group,
		// Everyone on the local machine.
		World,
	};
} // namespace datapath
