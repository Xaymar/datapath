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
#include "error.hpp"
#include "event.hpp"
#include "itask.hpp"

namespace datapath {
	class isocket {
		public /*events*/:
		datapath::event<const std::vector<char>&> on_message;

		datapath::event<datapath::error> on_close;

		public:
		virtual bool good() = 0;

		virtual datapath::error close() = 0;

		virtual datapath::error write(std::shared_ptr<datapath::itask>& task, const std::vector<char>& data) = 0;
	};
} // namespace datapath
