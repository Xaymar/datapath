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
#include <chrono>
#include <vector>
#include "error.hpp"
#include "event.hpp"

namespace datapath {
	class waitable {
		public /*events*/:
		datapath::event<datapath::error> on_wait_error;

		datapath::event<datapath::error> on_wait_success;

		public:
		virtual void* get_waitable() = 0;

		inline datapath::error wait(std::chrono::nanoseconds duration = std::chrono::nanoseconds(0))
		{
			return datapath::waitable::wait(this, duration);
		}

		public /*static*/:

		static datapath::error wait(datapath::waitable*      obj,
		                            std::chrono::nanoseconds duration = std::chrono::nanoseconds(0));

		static datapath::error wait(datapath::waitable** objs, size_t count,
		                            std::chrono::nanoseconds duration = std::chrono::nanoseconds(0));

		static inline datapath::error wait(std::vector<datapath::waitable*> objs,
		                                   std::chrono::nanoseconds duration = std::chrono::nanoseconds(0))
		{
			return datapath::waitable::wait(objs.data(), objs.size(), duration);
		}

		static datapath::error wait_any(datapath::waitable** objs, size_t count, size_t& index,
		                                std::chrono::nanoseconds duration = std::chrono::nanoseconds(0));

		static inline datapath::error wait_any(std::vector<datapath::waitable*> objs, size_t& index,
		                                       std::chrono::nanoseconds duration = std::chrono::nanoseconds(0))
		{
			return datapath::waitable::wait_any(objs.data(), objs.size(), index, duration);
		}
	};
} // namespace datapath
