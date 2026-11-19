/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <string>

namespace datapath::windows {
	// Turn a DataPath path into a wide named-pipe path. Bare names become
	// \\.\pipe\<name>; full \\.\pipe\ paths are kept. Returns false if invalid.
	bool make_pipe_path(const std::string& path, std::wstring& out);
} // namespace datapath::windows
