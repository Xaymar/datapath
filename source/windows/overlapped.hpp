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
#include <cinttypes>
#include <vector>
#include "waitable.hpp"

extern "C" {
#include <windows.h>
}

namespace datapath {
	namespace windows {
		class overlapped : public datapath::waitable {
			std::vector<char> buffer;
			OVERLAPPED*       overlapped_ptr;
			HANDLE            handle;
			void*             data;

			public:
			overlapped();
			~overlapped();

			OVERLAPPED* get_overlapped();

			HANDLE get_handle();
			void   set_handle(HANDLE handle);

			void* get_data();
			void  set_data(void* data);

			void cancel();

			bool is_completed();

			void reset();

			public /*virtual override*/:
			virtual void* get_waitable() override;
		};
	} // namespace windows
} // namespace datapath
