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
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include "iserver.hpp"
#include "permissions.hpp"

extern "C" {
#include <Windows.h>
}

namespace datapath {
	namespace windows {
		class server : public iserver {
			bool        is_created  = false;
			size_t      max_clients = -1;
			std::string path;

			private /*critical data*/:
			// Lock for critical data.
			std::mutex lock;

			SECURITY_ATTRIBUTES security_attributes;

			std::list<HANDLE>                                  sockets;
			std::list<HANDLE>                                  waiting_sockets;
			std::list<HANDLE>                                  pending_sockets;
			std::map<HANDLE, std::weak_ptr<datapath::isocket>> active_sockets;

			struct {
				std::thread task;
				std::mutex  lock;
				bool        shutdown = false;
			} watcher;

			protected:
			datapath::error create(std::string path, datapath::permissions permissions, size_t max_clients);

			HANDLE _create_socket(std::string path, bool initial = false);

			void _watcher();

			public /*virtual override*/:
			virtual datapath::error accept(std::shared_ptr<datapath::isocket>& socket) override;

			virtual datapath::error close() override;

			public:
			static datapath::error host(std::shared_ptr<datapath::iserver>& server, std::string path,
			                            datapath::permissions permissions, size_t max_clients);
		};
	} // namespace windows
} // namespace datapath