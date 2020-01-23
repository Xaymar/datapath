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

#include "datapath.hpp"

extern "C" {
#include <windows.h>
}

namespace datapath {
	namespace windows {
		class overlapped {
			std::vector<char> _buffer;
			OVERLAPPED*       _overlapped;
			HANDLE            _handle;

			std::shared_ptr<void>                           _data;
			std::function<void(overlapped&, size_t, void*)> _callback;

			public:
			overlapped();
			~overlapped();

			OVERLAPPED* get_overlapped();

			HANDLE get_handle();
			void   set_handle(HANDLE _handle);

			std::shared_ptr<void> get_data();
			void                  set_data(std::shared_ptr<void> _data);

			void set_callback(std::function<void(overlapped&, size_t, void*)> cb);
			void callback(size_t bytes, void* ptr);

			void cancel();

			bool is_completed();

			void reset();

			::datapath::error status();

			public:
			static overlapped* from_overlapped(OVERLAPPED* ptr)
			{
				return *reinterpret_cast<overlapped**>(reinterpret_cast<int8_t*>(ptr) + sizeof(OVERLAPPED));
			}
		};
	} // namespace windows
} // namespace datapath
