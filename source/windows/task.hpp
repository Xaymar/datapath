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
#include "itask.hpp"
#include "overlapped.hpp"
#include "socket.hpp"
#include "server.hpp"

extern "C" {
#include <Windows.h>
}

namespace datapath {
	namespace windows {
		class task : public itask {
			std::shared_ptr<datapath::windows::overlapped> overlapped;
			std::vector<char>                              buffer;

			protected:
			void _assign(const std::vector<char>& data, std::shared_ptr<datapath::windows::overlapped> ov);

			public:
			task();
			~task();

			public /*virtual override*/ /*itask*/:
			virtual datapath::error cancel() override;

			virtual bool is_completed() override;

			virtual size_t length() override;

			virtual const std::vector<char>& data() override;

			public /*virtual override*/ /*waitable*/:
			virtual void* get_waitable() override;

			friend class datapath::windows::socket;
			friend class datapath::windows::server;
		};
	} // namespace windows
} // namespace datapath
