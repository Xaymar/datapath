/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <cstdint>

namespace datapath::detail {
	inline void store_le64(uint8_t* p, uint64_t v) noexcept
	{
		for (int i = 0; i < 8; i++)
			p[i] = uint8_t(v >> (8 * i));
	}
	inline uint64_t load_le64(const uint8_t* p) noexcept
	{
		uint64_t v = 0;
		for (int i = 0; i < 8; i++)
			v |= uint64_t(p[i]) << (8 * i);
		return v;
	}
	inline void store_le32(uint8_t* p, uint32_t v) noexcept
	{
		for (int i = 0; i < 4; i++)
			p[i] = uint8_t(v >> (8 * i));
	}
	inline uint32_t load_le32(const uint8_t* p) noexcept
	{
		uint32_t v = 0;
		for (int i = 0; i < 4; i++)
			v |= uint32_t(p[i]) << (8 * i);
		return v;
	}
} // namespace datapath::detail
