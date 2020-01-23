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
#include <atomic>
#include <queue>
#include <string>
#include <utility>

#include "datapath.hpp"
#include "windows-overlapped.hpp"
#include "windows-server.hpp"
#include "windows-socket.hpp"

namespace datapath::windows {
	class server_socket : public ::datapath::windows::socket {
		// Data
		std::mutex            _lock;
		std::atomic_bool      _opened;
		std::weak_ptr<server> _parent;

		// Windows
		std::shared_ptr<void> _handle;
		std::shared_ptr<void> _iocp;

		// Asynchronous Operation
		::datapath::windows::overlapped _ov_open;

		// Parent/Child relationship

		public:
		server_socket(std::shared_ptr<::datapath::windows::server> parent, bool initial);
		virtual ~server_socket();

		public /* Virtual Implementations */:
		virtual void set_path(std::string path) override;

		virtual void open() override;

		virtual void close() override;

		virtual bool is_open() override;

		virtual void work(std::chrono::milliseconds time_limit) override;

		private:
		void on_open(::datapath::windows::overlapped& ov, std::size_t size, void* ptr);

		protected:
		friend class ::datapath::windows::server;
	};
} // namespace datapath::windows
