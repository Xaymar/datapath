/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <datapath/permissions.hpp>
#include <string>
#include "win.hpp"

namespace datapath::windows {
	// SDDL string for a permission level, for the given user SID string.
	std::wstring make_sddl(datapath::permissions level, const std::wstring& user_sid);
	// The current process user's SID as a string, or empty on failure.
	std::wstring current_user_sid();
	// Security descriptor for named pipes; free with LocalFree. nullptr on failure.
	PSECURITY_DESCRIPTOR make_security_descriptor(datapath::permissions level);
} // namespace datapath::windows
