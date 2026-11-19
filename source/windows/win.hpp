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

// The only place Windows headers are included from.

#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <datapath/error.hpp>

namespace datapath::windows {
	// Map a Win32 error from a pipe operation to a DataPath error.
	inline datapath::error map_error(DWORD err) noexcept
	{
		switch (err) {
		case ERROR_SUCCESS:
			return datapath::error::Success;
		case ERROR_BROKEN_PIPE:
		case ERROR_NO_DATA:
		case ERROR_PIPE_NOT_CONNECTED:
		case ERROR_OPERATION_ABORTED:
		case ERROR_INVALID_HANDLE:
		case ERROR_NETNAME_DELETED:
			return datapath::error::Closed;
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
			return datapath::error::NotFound;
		case ERROR_ACCESS_DENIED:
			return datapath::error::AccessDenied;
		case ERROR_SEM_TIMEOUT:
			return datapath::error::TimedOut;
		default:
			return datapath::error::Failure;
		}
	}
} // namespace datapath::windows
