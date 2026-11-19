/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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
