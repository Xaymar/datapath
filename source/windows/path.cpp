/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "path.hpp"
#include "win.hpp"

bool datapath::windows::make_pipe_path(const std::string& path, std::wstring& out)
{
	static const std::string prefix = "\\\\.\\pipe\\";
	std::string              full;
	if (path.compare(0, prefix.size(), prefix) == 0) {
		full = path;
	} else {
		full = path;
		for (char& c : full)
			if (c == '\\')
				c = '/';
		full = prefix + full;
	}
	if (full.size() == prefix.size() || full.size() > 256)
		return false;

	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, full.data(), int(full.size()), nullptr, 0);
	if (n <= 0)
		return false;
	out.resize(size_t(n));
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, full.data(), int(full.size()), &out[0], n);
	return true;
}
