/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 Michael Fabian Dirks <info@xaymar.com>

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
		// Unknown error.
		Unknown = -1,

		// Operation was successful.
		Success,

		// Operation failed with one or more recoverable errors.
		Failure,

		// Operation failed with one or more unrecoverable errors. The object is now in an undetermined state.
		CriticalFailure,

		// Operation timed out.
		TimedOut,

		// Operation is not supported.
		NotSupported,

		// Socket Closed
		SocketClosed,

		// The given path is invalid.
		InvalidPath,

		// The header sent by the remote was malformed or corrupted.
		BadHeader,

		// The size included in the header is too big or invalid.
		BadSize,
	};
}
