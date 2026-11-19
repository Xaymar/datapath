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
