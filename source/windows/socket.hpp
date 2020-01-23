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
#include <thread>
#include <vector>
#include "event.hpp"
#include "isocket.hpp"
#include "overlapped-queue.hpp"
#include "overlapped.hpp"
#include "server.hpp"

extern "C" {
#include <Windows.h>
}

namespace datapath {
	namespace windows {
		class socket : public isocket, public std::enable_shared_from_this<datapath::windows::socket> {
			bool   is_connected;
			HANDLE socket_handle;

			struct {
				std::thread task;
				std::mutex  lock;
				bool        shutdown = false;
			} watcher;

			protected:
			void _connect(HANDLE handle);

			void _disconnect();

			void _watcher();

			public:
			socket();

			virtual ~socket();

			public /*virtual override*/:
			virtual bool good() override;

			virtual datapath::error close() override;

			virtual datapath::error write(std::shared_ptr<datapath::itask>& task,
										  const std::vector<char>&          data) override;

			public:
			static datapath::error connect(std::shared_ptr<datapath::isocket>& socket, std::string path);

			friend class datapath::windows::server;
		};
	} // namespace windows
} // namespace datapath
