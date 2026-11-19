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

// Connection handshake: "DPTH", uint32 version, uint64 max_message_size (little-endian).

#pragma once
#include <datapath/error.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "endian.hpp"

namespace datapath::detail {
	constexpr uint32_t protocol_version = 1;
	constexpr size_t   hello_size       = 16;

	struct hello {
		uint32_t version;
		uint64_t max_message_size;
	};

	inline void encode_hello(uint8_t* out, const hello& h) noexcept
	{
		std::memcpy(out, "DPTH", 4);
		store_le32(out + 4, h.version);
		store_le64(out + 8, h.max_message_size);
	}

	inline datapath::error decode_hello(const uint8_t* in, hello& out) noexcept
	{
		if (std::memcmp(in, "DPTH", 4) != 0)
			return datapath::error::VersionMismatch;
		out.version          = load_le32(in + 4);
		out.max_message_size = load_le64(in + 8);
		if (out.version != protocol_version)
			return datapath::error::VersionMismatch;
		return datapath::error::Success;
	}
} // namespace datapath::detail
