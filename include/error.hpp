/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019  Michael Fabian Dirks <info@xaymar.com>

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
#include <cinttypes>

namespace datapath {
	enum class error : int32_t {
		// Unknown
		Unknown = -1,

		// Success
		Success,

		// Failure (Generic)
		Failure,

		// Failure (Critical Generic)
		CriticalFailure,

		// Socket Closed
		Closed,

		// Timed Out
		TimedOut,

		// Invalid Path
		InvalidPath,
	};
}
