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
