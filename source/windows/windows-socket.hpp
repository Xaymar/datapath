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
#include <memory>
#include <mutex>
#include <queue>

#include "datapath.hpp"
#include "socket.hpp"
#include "windows-overlapped.hpp"

namespace datapath::windows {
	typedef uint32_t packet_size_t;

	class socket : public ::datapath::socket, public std::enable_shared_from_this<::datapath::windows::socket> {
		typedef std::pair<::datapath::io_callback_t, ::datapath::io_callback_data_t> read_data_t;
		typedef std::tuple<::datapath::io_data_t, ::datapath::io_callback_t, ::datapath::io_callback_data_t>
			write_data_t;

		// Read Data
		std::recursive_mutex    _read_queue_lock;
		std::queue<read_data_t> _read_queue;
		io_data_t               _read_buffer;

		// Write Data
		std::recursive_mutex     _write_queue_lock;
		std::queue<write_data_t> _write_queue;

		protected: // Asynchronous IO
		::datapath::windows::overlapped _ov_read;
		::datapath::windows::overlapped _ov_write;

		public /* Input/Output */:
		virtual void read(::datapath::io_callback_t callback, ::datapath::io_callback_data_t callback_data) override;

		virtual void write(const io_data_t& data, ::datapath::io_callback_t callback,
						   ::datapath::io_callback_data_t callback_data) override;

		virtual void write(const std::uint8_t* data, const size_t data_length, ::datapath::io_callback_t callback,
						   ::datapath::io_callback_data_t callback_data) override;

		public /* Internal Events */:
		struct {
			::datapath::event<::datapath::error, std::shared_ptr<::datapath::socket>> opened;

			::datapath::event<::datapath::error, std::shared_ptr<::datapath::socket>> closed;
		} internal_events;

		private:
		::datapath::error queue_read(::datapath::io_callback_t cb, ::datapath::io_callback_data_t cbd);

		::datapath::error queue_write(const std::uint8_t* data, std::size_t length, ::datapath::io_callback_t cb,
									  ::datapath::io_callback_data_t cbd);

		::datapath::error perform_read();

		::datapath::error perform_read_packet(packet_size_t size);

		::datapath::error perform_write();

		void on_read_header(::datapath::windows::overlapped& ov, std::size_t size, void* ptr);
		void on_read(::datapath::windows::overlapped& ov, std::size_t size, void* ptr);
		void on_write(::datapath::windows::overlapped& ov, std::size_t size, void* ptr);
	};
} // namespace datapath::windows
