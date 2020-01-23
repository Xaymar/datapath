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
#include <memory>
#include "error.hpp"
#include "event.hpp"
#include "isocket.hpp"

namespace datapath {
	class iserver {
		public /*event*/:

		/** Accepted Connection Event
		 * This event is called if a new connection is pending evaluation. 
		 * 
		 * @param bool& Set to true to accept, false to decline.
		 * @param std::shared_ptr<datapath::isocket> Socket.
		 * @return void
		 */
		datapath::event<bool&, std::shared_ptr<datapath::isocket>> on_accept;

		public:
		virtual datapath::error close() = 0;
	};
} // namespace datapath
