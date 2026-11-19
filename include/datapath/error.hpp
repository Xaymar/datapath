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

#pragma once
#include <cstdint>

namespace datapath {
	enum class error : int32_t {
		Unknown = -1,
		Success,
		// Generic failure; details were not mappable to a specific code.
		Failure,
		CriticalFailure,
		// The connection or server is closed.
		Closed,
		TimedOut,
		// The path is malformed or too long for the platform.
		InvalidPath,
		NotSupported,
		// Message exceeds the negotiated maximum message size.
		TooLarge,
		// The send queue is full; retry later.
		WouldBlock,
		// The peer speaks a different protocol version.
		VersionMismatch,
		// Nothing is listening at the path.
		NotFound,
		// Another server already hosts this path.
		AddressInUse,
		AccessDenied,
	};

	const char* to_string(error e) noexcept;
} // namespace datapath
