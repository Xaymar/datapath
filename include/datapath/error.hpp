/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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
