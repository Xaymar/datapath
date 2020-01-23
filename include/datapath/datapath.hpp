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
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "datapath/config.hpp"
#include "datapath/error.hpp"

namespace datapath {
	constexpr std::size_t maximum_packet_size = 1048576;

	class server;
	class socket;

	typedef std::vector<std::uint8_t> io_data_t;
	typedef std::shared_ptr<void>     io_callback_data_t;

	typedef std::function<void(std::shared_ptr<::datapath::socket>, ::datapath::error, const ::datapath::io_data_t&,
							   ::datapath::io_callback_data_t)>
		io_callback_t;
} // namespace datapath
