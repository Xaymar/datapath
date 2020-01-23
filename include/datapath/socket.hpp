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
#include <string>

#include "datapath.hpp"
#include "event.hpp"

namespace datapath {
	class DATAPATH_INTERFACE socket {
		public:
		/*!
		 * Create a new socket for inter-process communications.
		 *
		 * @return A new socket object ready to be used.
		 */
		static std::shared_ptr<::datapath::socket> create();

		public /* Configuration */:
		/*!
		 * Changes the path for the socket to connect to. This path must have a server listening on it.
		 * 
		 * @param path				Unique path for the socket to connect to.
		 * @throws TODO
		 */
		virtual void set_path(std::string path) = 0;

		public /* State Change */:
		/*!
		 * Try to connect to the given path, which must have a server listening on it. If the socket was already
		 * connected, it will first disconnect and then connect again.
		 * 
		 * @throws TODO
		 */
		virtual void open() = 0;

		/*!
		 * Disconnect from the currently connected server, if there are any.
		 * 
		 * @throws TODO
		 */
		virtual void close() = 0;

		public /* State Checking */:
		/*!
		 * Check if the socket is currently open or not.
		 * 
		 * @return `true` if the scoket is open, otherwise returns `false`.
		 */
		virtual bool is_open() = 0;

		public /* Concurrency */:
		/*!
		 * Perform any work that is pending.
		 *
		 * By default the socket does not create any threads to do work for it, in order to maintain compatibility with
		 * all platforms. So you have to call this from your own code in order to make anything actually happen.
		 *
		 * Note: This has no effect when called on a socket created from a server.
		 * 
		 * @param time_limit	Optional time limit for the work (only if platform supports it).
		 * @throws TODO
		 */
		virtual void work(std::chrono::milliseconds time_limit = std::chrono::milliseconds(0xFFFFFFFFFF)) = 0;

		public /* Input/Output */:
		/*!
		 * Enqueue an asynchronous read operation from this socket.
		 *
		 * @param callback			The callback function to call when this read operation is executed.
		 * @param callback_data		Data to pass to the callback.
		 * @throws TODO
		 **/
		virtual void read(::datapath::io_callback_t callback, ::datapath::io_callback_data_t callback_data) = 0;

		/*!
		 * Enqueue an asynchronous write operation to this socket.
		 *
		 * @param data				Data to write to the socket.
		 * @param callback			The callback function to call when this write operation is executed.
		 * @param callback_data		Data to pass to the callback.
		 * @throws TODO
		 **/
		virtual void write(const io_data_t& data, ::datapath::io_callback_t callback,
						   ::datapath::io_callback_data_t callback_data) = 0;

		/*!
		 * Enqueue an asynchronous write operation to this socket.
		 *
		 * @param data				Data to write to the socket.
		 * @param data_length		Length of the data to write to the socket.
		 * @param callback			The callback function to call when this write operation is executed.
		 * @param callback_data		Data to pass to the callback.
		 * @throws TODO
		 **/
		virtual void write(const std::uint8_t* data, const size_t data_length, ::datapath::io_callback_t callback,
						   ::datapath::io_callback_data_t callback_data) = 0;

		public:
		struct {
			::datapath::event<::datapath::error, std::shared_ptr<::datapath::socket>> opened;

			::datapath::event<::datapath::error, std::shared_ptr<::datapath::socket>> closed;
		} events;
	};
} // namespace datapath
