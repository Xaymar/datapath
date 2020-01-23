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
#include "waitable.hpp"

namespace datapath {
	class itask : public waitable {
		public /*event*/:
		datapath::event<datapath::error> _on_failure;

		datapath::event<datapath::error, const std::vector<char>&> _on_success;

		public:
		virtual datapath::error cancel() = 0;

		virtual bool is_completed() = 0;

		virtual size_t length() = 0;

		virtual const std::vector<char>& data() = 0;
	};
} // namespace datapath
