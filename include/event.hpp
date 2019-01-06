/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019  Michael Fabian Dirks <info@xaymar.com>

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
#include <functional>
#include <list>

namespace datapath {
	template<typename... _args>
	class event {
		std::list<std::function<void(_args...)>> listeners;

		public:
		void add(std::function<void(_args...)> listener)
		{
			listeners.push_back(listener);
		}

		void remove(std::function<void(_args...)> listener)
		{
			listeners.remove(listener);
		}

		// Not valid without the extra template.
		template<typename... _largs>
		void operator()(_largs... args)
		{
			for (auto& l : listeners) {
				l(args...);
			}
		}

		operator bool()
		{
			return !listeners.empty();
		}

		public:
		bool empty()
		{
			return listeners.empty();
		}

		void clear()
		{
			listeners.clear();
		}
	};
}; // namespace datapath
