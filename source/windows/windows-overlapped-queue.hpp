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

#include <cinttypes>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include "overlapped.hpp"

extern "C" {
#include <AccCtrl.h>
#include <AclAPI.h>
#include <windows.h>
}

namespace datapath {
	namespace windows {
		class overlapped_queue {
			std::queue<std::shared_ptr<datapath::windows::overlapped>> free_objs;
			std::list<std::shared_ptr<datapath::windows::overlapped>>  used_objs;
			std::mutex                                                 objs_lock;

			public:
			overlapped_queue(size_t backlog = 8);

			virtual ~overlapped_queue();

			std::shared_ptr<datapath::windows::overlapped> alloc();

			void free(std::shared_ptr<datapath::windows::overlapped> overlapped);
		};
	} // namespace windows
} // namespace datapath
