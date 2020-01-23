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
#include <cstdint>
#include <memory>
#include <string>

#include "datapath.hpp"
#include "event.hpp"
#include "socket.hpp"

namespace datapath {
	class DATAPATH_INTERFACE server {
		public:
		/*!
		 * Create a new server for inter-process communications.
		 *
		 * @return A new server object ready to be used.
		 * @throws TODO
		 */
		static std::shared_ptr<::datapath::server> create();

		public /* Configuration */:
		/*!
		 * Changes the path for the server to listen on. The path must be unique and not in use by another process or
		 * server. This path is what sockets have to connect to.
		 * 
		 * @param path				Unique path for the server to be listening on.
		 * @throws TODO
		 */
		virtual void set_path(std::string path) = 0;

		public /* State Change */:
		/*!
		 * Start listening for connections from clients on the set path, closing the server if it was open at the time
		 * of calling this.
		 * 
		 * @throws TODO
		 */
		virtual void open() = 0;

		/*!
		 * Stop listening for connections from clients. Does nothing if already closed.
		 * 
		 * @throws TODO
		 */
		virtual void close() = 0;

		public /* State Checking */:
		/*!
		 * Check if the server is currently open or not.
		 * 
		 * @return `true` if the server is open, otherwise returns `false`.
		 */
		virtual bool is_open() = 0;

		public /* Concurrency */:
		/*!
		 * Perform any work that is pending.
		 *
		 * By default the server does not create any threads to do work for it, in order to maintain compatibility with
		 * all platforms. So you have to call this from your own code in order to make anything actually happen.
		 * 
		 * @param time_limit	Optional time limit for the work (only if platform supports it).
		 * @throws TODO
		 */
		virtual void work(std::chrono::milliseconds time_limit = std::chrono::milliseconds(0xFFFFFFFFFF)) = 0;

		public:
		struct {
			/*!
			 * Event for new client connections, called whenever a client is connecting.
			 * 
			 * @param bool&		Set to 'true' to allow the connection, or 'false' to reject.
			 * @param std::shared_ptr<datapath::socket> The actual socket, must be managed.
			 * @return void
			 */
			::datapath::event<bool&, std::shared_ptr<::datapath::socket>> connected;
		} events;
	};
} // namespace datapath
