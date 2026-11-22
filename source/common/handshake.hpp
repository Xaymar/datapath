/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Connection handshake: "DPTH", uint32 version, uint64 max_message_size (little-endian).
//
// Version 2 (Windows, opt-in core pairing): a client whose context has
// pair_peer sends version 2. A server that knows version 2 answers with a
// version 2 hello followed by a pair trailer: int32 logical processor for the
// client's I/O thread (-1 = no pairing), uint32 reserved. A server that only
// knows version 1 answers with version 1 and closes; the client then connects
// again with version 1.
//
// Version 3 (Windows, extensions): the client sends a version 3 hello followed
// by a request trailer: uint32 flags (ext_pair, ext_shared_memory), uint32
// reserved. A server that knows version 3 answers with a version 3 hello and a
// reply trailer: int32 pair processor (-1 = none), uint32 accepted flags. When
// ext_shared_memory was accepted, a shared-memory offer follows (see
// source/windows/shm.hpp). Servers that don't know version 3 answer with
// version 1 and close; the client then falls back to version 2 or 1.

#pragma once
#include <datapath/error.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "endian.hpp"

namespace datapath::detail {
	constexpr uint32_t protocol_version      = 1;
	constexpr uint32_t protocol_version_pair = 2;
	constexpr size_t   hello_size            = 16;
	constexpr size_t   pair_trailer_size     = 8;
	constexpr uint32_t protocol_version_ext  = 3;
	constexpr size_t   ext_trailer_size      = 8;
	constexpr uint32_t ext_pair              = 1;
	constexpr uint32_t ext_shared_memory     = 2;

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

	// Like decode_hello, but also accepts version 2 (core pairing).
	inline datapath::error decode_hello_v2(const uint8_t* in, hello& out) noexcept
	{
		if (std::memcmp(in, "DPTH", 4) != 0)
			return datapath::error::VersionMismatch;
		out.version          = load_le32(in + 4);
		out.max_message_size = load_le64(in + 8);
		if (out.version != protocol_version && out.version != protocol_version_pair)
			return datapath::error::VersionMismatch;
		return datapath::error::Success;
	}

	// Accepts versions 1 to 3.
	inline datapath::error decode_hello_any(const uint8_t* in, hello& out) noexcept
	{
		if (std::memcmp(in, "DPTH", 4) != 0)
			return datapath::error::VersionMismatch;
		out.version          = load_le32(in + 4);
		out.max_message_size = load_le64(in + 8);
		if (out.version < protocol_version || out.version > protocol_version_ext)
			return datapath::error::VersionMismatch;
		return datapath::error::Success;
	}

	// Version 3 trailers. Request: flags, reserved. Reply: pair processor, flags.
	inline void encode_ext_trailer(uint8_t* out, uint32_t first, uint32_t second) noexcept
	{
		store_le32(out, first);
		store_le32(out + 4, second);
	}

	inline void decode_ext_trailer(const uint8_t* in, uint32_t& first, uint32_t& second) noexcept
	{
		first  = load_le32(in);
		second = load_le32(in + 4);
	}

	inline void encode_pair_trailer(uint8_t* out, int32_t cpu) noexcept
	{
		store_le32(out, uint32_t(cpu));
		store_le32(out + 4, 0);
	}

	inline int32_t decode_pair_trailer(const uint8_t* in) noexcept
	{
		return int32_t(load_le32(in));
	}
} // namespace datapath::detail
