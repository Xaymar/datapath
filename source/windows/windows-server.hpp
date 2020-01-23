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
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include "server.hpp"
#include "windows-overlapped.hpp"

extern "C" {
#include <Windows.h>
}

// Server is simple with IOCP:
// - Create 1 IOCP per Server.
// - Create a ThreadPool per IOCP.
// - Spawn x Tasks to fill the ThreadPool and wait on IOCP, handler simply calls callbacks in OVERLAPPED.
// - Create x Sockets (up to Hardware Concurrency) ahead of time.
// - Create x Clients ahead of time (equal to Sockets).
// - When a client connects, fill up the free sockets list with a new socket (up to Hardware Concurrency).
// - When a client disconnects, return the socket to the free state and reset the client.

namespace datapath::windows {
	class server_socket;

	class server : public ::datapath::server, public std::enable_shared_from_this<datapath::windows::server> {
		// Data
		std::mutex         _lock;
		std::atomic_bool   _opened;
		std::wstring       _path;
		std::atomic_size_t _worker_count;

		// Sockets
		std::list<std::shared_ptr<::datapath::windows::server_socket>> _sockets;
		std::atomic_size_t                                             _sockets_free;

		// Windows
		std::shared_ptr<void> _iocp;

		public:
		server();
		virtual ~server();

		// Move/Copy Operator
		server(const server&) = delete;
		server& operator=(const server&) = delete;

		public /* Virtual Implementations */:
		virtual void set_path(std::string path) override;

		virtual void open() override;

		virtual void close() override;

		virtual bool is_open() override;

		virtual void work(std::chrono::milliseconds time_limit) override;

		private:
		std::shared_ptr<::datapath::windows::server_socket> create_socket();

		void on_socket_opened(::datapath::error error, std::shared_ptr<::datapath::socket> socket);

		void on_socket_closed(::datapath::error error, std::shared_ptr<::datapath::socket> socket);

		protected /* Socket <-> Server Interface */:
		std::wstring& path();

		std::shared_ptr<void> iocp();

		friend class ::datapath::windows::server_socket;
	};
} // namespace datapath::windows
