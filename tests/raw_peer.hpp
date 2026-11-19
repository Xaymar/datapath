/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Raw platform connections used by tests to play a misbehaving peer.

#pragma once
#include <cstddef>
#include <memory>
#include <string>

namespace raw {
	struct peer;
	struct peer_deleter {
		void operator()(peer* p) const;
	};
	using peer_ptr = std::unique_ptr<peer, peer_deleter>;

	// Connect to the DataPath server hosted at `name` (a bare name) without a handshake.
	peer_ptr connect(const std::string& name);
	bool     write(peer& p, const void* data, size_t size);
	// Read exactly `size` bytes; false on timeout, EOF or error.
	bool read(peer& p, void* data, size_t size, int timeout_ms);
	// True if the peer closed its end within `timeout_ms`.
	bool wait_closed(peer& p, int timeout_ms);
} // namespace raw
