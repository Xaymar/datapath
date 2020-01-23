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

#include "windows-socket.hpp"

void datapath::windows::socket::read(::datapath::io_callback_t callback, ::datapath::io_callback_data_t callback_data)
{
	auto res = queue_read(callback, callback_data);
	switch (res) {
	case error::Success:
		return;
	case error::SocketClosed: {
		auto status = ::datapath::error::SocketClosed;
		auto self   = shared_from_this();
		events.closed(status, self);
		internal_events.closed(status, self);
		throw std::runtime_error("Socket closed");
	}
	default:
		throw std::runtime_error("TODO");
	}
}

void datapath::windows::socket::write(const ::datapath::io_data_t& data, ::datapath::io_callback_t callback,
									  ::datapath::io_callback_data_t callback_data)
{
	auto res = queue_write(data.data(), data.size(), callback, callback_data);
	switch (res) {
	case error::Success:
		return;
	case error::SocketClosed: {
		auto status = ::datapath::error::SocketClosed;
		auto self   = shared_from_this();
		events.closed(status, self);
		internal_events.closed(status, self);
		throw std::runtime_error("Socket closed");
	}
	default:
		throw std::runtime_error("TODO");
	}
}

void datapath::windows::socket::write(const std::uint8_t* data, const size_t data_length,
									  ::datapath::io_callback_t callback, ::datapath::io_callback_data_t callback_data)
{
	auto res = queue_write(data, data_length, callback, callback_data);
	switch (res) {
	case error::Success:
		return;
	case error::SocketClosed: {
		auto status = ::datapath::error::SocketClosed;
		auto self   = shared_from_this();
		events.closed(status, self);
		internal_events.closed(status, self);
		throw std::runtime_error("Socket closed");
	}
	default:
		throw std::runtime_error("TODO");
	}
}

::datapath::error datapath::windows::socket::queue_read(::datapath::io_callback_t      cb,
														::datapath::io_callback_data_t cbd)
{
	// Early-Exit if the socket is already closed.
	if (!is_open())
		return ::datapath::error::NotSupported;

	// Prevent multiple thread from modifying the same objects.
	std::lock_guard<std::recursive_mutex> lg(_read_queue_lock);

	// Queue the read operation.
	_read_queue.push(read_data_t{cb, cbd});

	// If this is the only operation, instantly perform it.
	if (_read_queue.size() == 1) {
		return perform_read();
	} else {
		return ::datapath::error::Success;
	}
}

::datapath::error datapath::windows::socket::queue_write(const std::uint8_t* data, std::size_t length,
														 ::datapath::io_callback_t      cb,
														 ::datapath::io_callback_data_t cbd)
{
	// Early-Exit if the socket is already closed.
	if (!is_open())
		return ::datapath::error::NotSupported;

	// Build actual packet.
	io_data_t packet;
	packet.resize(length + sizeof(packet_size_t));
	memcpy(packet.data() + sizeof(packet_size_t), data, length);
	*reinterpret_cast<packet_size_t*>(packet.data()) = length;

	// Prevent multiple thread from modifying the same objects.
	std::lock_guard<std::recursive_mutex> lg(_write_queue_lock);

	// Queue the read request.
	_write_queue.push(std::move(write_data_t{std::move(packet), cb, cbd}));

	// If this is the only operation, instantly perform it.
	if (_write_queue.size() == 1) {
		return perform_write();
	} else {
		return ::datapath::error::Success;
	}
}

::datapath::error datapath::windows::socket::perform_read()
{
	DWORD bytes;

	// Prevent multiple thread from modifying the same objects.
	std::lock_guard<std::recursive_mutex> lg(_read_queue_lock);

	// Early-exit if there is nothing to do.
	if (_read_queue.size() == 0)
		return ::datapath::error::Failure;

	// Resize buffer, then issue read request.
	_read_buffer.resize(sizeof(uint32_t));
	_ov_read.reset();
	_ov_read.set_callback(std::bind(&::datapath::windows::socket::on_read_header, this, std::placeholders::_1,
									std::placeholders::_2, std::placeholders::_3));

	// Issue read request (return value can be ignored, it will always be false).
	SetLastError(ERROR_SUCCESS);
	DWORD res =
		ReadFile(_ov_read.get_handle(), _read_buffer.data(), _read_buffer.size(), &bytes, _ov_read.get_overlapped());

	// Report result to caller.
	DWORD ec = GetLastError();
	switch (ec) {
	case ERROR_SUCCESS:
	case ERROR_IO_PENDING:
		return ::datapath::error::Success;
	case ERROR_PIPE_NOT_CONNECTED:
	case ERROR_BROKEN_PIPE:
		return ::datapath::error::SocketClosed;
	default:
		return ::datapath::error::Failure;
	}
}

::datapath::error datapath::windows::socket::perform_read_packet(packet_size_t size)
{
	DWORD bytes;

	// Prevent multiple thread from modifying the same objects.
	std::lock_guard<std::recursive_mutex> lg(_read_queue_lock);

	// Early-exit if there is nothing to do.
	if (_read_queue.size() == 0)
		return ::datapath::error::Failure;

	// Resize Buffer, then issue the 2nd read request.
	_read_buffer.resize(size);
	_ov_read.reset();
	_ov_read.set_callback(std::bind(&::datapath::windows::socket::on_read, this, std::placeholders::_1,
									std::placeholders::_2, std::placeholders::_3));

	// Issue read request (return value can be ignored, it will always be false).
	SetLastError(ERROR_SUCCESS);
	DWORD res =
		ReadFile(_ov_read.get_handle(), _read_buffer.data(), _read_buffer.size(), &bytes, _ov_read.get_overlapped());

	// Report result to caller.
	DWORD ec = GetLastError();
	switch (ec) {
	case ERROR_SUCCESS:
	case ERROR_IO_PENDING:
		return ::datapath::error::Success;
	case ERROR_PIPE_NOT_CONNECTED:
	case ERROR_BROKEN_PIPE:
		return ::datapath::error::SocketClosed;
	default:
		return ::datapath::error::Failure;
	}
}

::datapath::error datapath::windows::socket::perform_write()
{
	DWORD bytes;

	// Prevent multiple thread from modifying the same objects.
	std::lock_guard<std::recursive_mutex> lg(_write_queue_lock);

	// Early-exit if there is nothing to do.
	if (_write_queue.size() == 0)
		return ::datapath::error::Failure;

	// Lock the queue, and grab the front element.
	auto& front = _write_queue.front();
	auto& data  = std::get<io_data_t>(front);

	// Reset the overlapped object.
	_ov_write.reset();

	// Issue write request (return value can be ignored, always false).
	SetLastError(ERROR_SUCCESS);
	DWORD res = WriteFile(_ov_write.get_handle(), data.data(), data.size(), &bytes, _ov_write.get_overlapped());

	// Report result to caller.
	DWORD ec = GetLastError();
	switch (ec) {
	case ERROR_SUCCESS:
	case ERROR_IO_PENDING:
		return ::datapath::error::Success;
	case ERROR_PIPE_NOT_CONNECTED:
	case ERROR_BROKEN_PIPE:
		return ::datapath::error::SocketClosed;
	default:
		return ::datapath::error::Failure;
	}
}

void datapath::windows::socket::on_read_header(::datapath::windows::overlapped& ov, std::size_t bytes_read, void* ptr)
{
	io_data_t data;

	// Sanity Check: Did we actually read the entire header?
	if (bytes_read != sizeof(packet_size_t)) {
		// Assume remote is a bad actor, or system is in a bad state.
		{
			std::lock_guard<std::recursive_mutex> lg(_read_queue_lock);

			auto el = std::move(_read_queue.front());
			if (el.first) {
				el.first(shared_from_this(), ::datapath::error::BadHeader, data, el.second);
			}
			_read_queue.pop();
		}
		close();
		return;
	}

	// Read the given size.
	packet_size_t size = *reinterpret_cast<packet_size_t*>(_read_buffer.data());

	// Sanity Check: Is the packet bigger than we allow it to be?
	if (size > ::datapath::maximum_packet_size) {
		// Soft-fail, remote may be testing our capabilities, is outdated, or may be newer than us.
		std::lock_guard<std::recursive_mutex> lg(_read_queue_lock);

		auto el = std::move(_read_queue.front());
		if (el.first) {
			el.first(shared_from_this(), ::datapath::error::BadSize, data, el.second);
		}
		_read_queue.pop();

		switch (perform_read()) {
		case error::SocketClosed: {
			auto status = ::datapath::error::SocketClosed;
			auto self   = shared_from_this();
			events.closed(status, self);
			internal_events.closed(status, self);
			return;
		}
		default:
			return;
		}
	}

	switch (perform_read_packet(size)) {
	case error::SocketClosed: {
		auto status = ::datapath::error::SocketClosed;
		auto self   = shared_from_this();
		events.closed(status, self);
		internal_events.closed(status, self);
		return;
	}
	default:
		return;
	}
}

void datapath::windows::socket::on_read(::datapath::windows::overlapped& ov, std::size_t size, void* ptr)
{ // Read completed, hopefully.
	::datapath::error status = ov.status();
	read_data_t       el;
	io_data_t         data{_read_buffer.data(), _read_buffer.data() + _read_buffer.size()};

	// Lock the queue from outside modifications.
	{
		std::lock_guard<std::recursive_mutex> lg(_read_queue_lock);
		el = std::move(_read_queue.front());
		_read_queue.pop();

		switch (perform_read()) {
		case error::SocketClosed: {
			auto status = ::datapath::error::SocketClosed;
			auto self   = shared_from_this();
			events.closed(status, self);
			internal_events.closed(status, self);
			return;
		}
		default:
			return;
		}
	}

	// Call the callback (if there is one).
	if (el.first) {
		el.first(shared_from_this(), ::datapath::error::Success, data, el.second);
	}
}

void datapath::windows::socket::on_write(::datapath::windows::overlapped& ov, std::size_t size, void* ptr)
{
	// Write completed. Hopefully.

	::datapath::error status = ov.status();
	write_data_t      el;

	{ // Remove from queue, and spawn new write request if something is still queued.
		std::lock_guard<std::recursive_mutex> lg(_write_queue_lock);
		el = std::move(_write_queue.front());
		_write_queue.pop();

		switch (perform_write()) {
		case error::SocketClosed: {
			auto status = ::datapath::error::SocketClosed;
			auto self   = shared_from_this();
			events.closed(status, self);
			internal_events.closed(status, self);
			return;
		}
		default:
			return;
		}
	}

	// Call callback.
	std::get<::datapath::io_callback_t>(el)(shared_from_this(), status, std::get<::datapath::io_data_t>(el),
											std::get<::datapath::io_callback_data_t>(el));
}
