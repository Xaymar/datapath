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

#include "windows-overlapped.hpp"

datapath::windows::overlapped::overlapped() : _overlapped(nullptr), _data(nullptr)
{
	size_t memory_size = sizeof(OVERLAPPED) + sizeof(void*);
	_buffer.resize(memory_size);

	reinterpret_cast<void*&>(_buffer[sizeof(OVERLAPPED)]) = this;
	_handle                                               = INVALID_HANDLE_VALUE;

	// Initialize OVERLAPPED
	_overlapped = &reinterpret_cast<OVERLAPPED&>(_buffer[0]);
	memset(_overlapped, 0, sizeof(OVERLAPPED));
	//overlapped_ptr->hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
}

datapath::windows::overlapped::~overlapped()
{
	cancel();
}

OVERLAPPED* datapath::windows::overlapped::get_overlapped()
{
	return _overlapped;
}

HANDLE datapath::windows::overlapped::get_handle()
{
	return _handle;
}

void datapath::windows::overlapped::set_handle(HANDLE handle)
{
	_handle = handle;
}

std::shared_ptr<void> datapath::windows::overlapped::get_data()
{
	return _data;
}

void datapath::windows::overlapped::set_data(std::shared_ptr<void> data)
{
	_data = data;
}

void datapath::windows::overlapped::set_callback(std::function<void(overlapped&, size_t, void*)> cb)
{
	_callback = cb;
}

void datapath::windows::overlapped::callback(size_t bytes, void* ptr)
{
	if (_callback)
		_callback(*this, bytes, ptr);
}

void datapath::windows::overlapped::cancel()
{
	CancelIoEx(_handle, _overlapped);
}

bool datapath::windows::overlapped::is_completed()
{
	return HasOverlappedIoCompleted(_overlapped);
}

void datapath::windows::overlapped::reset()
{
	cancel();
	_overlapped->Pointer = _overlapped->hEvent = nullptr;
	_overlapped->Internal = _overlapped->InternalHigh = _overlapped->Offset = _overlapped->OffsetHigh = 0;
}

::datapath::error datapath::windows::overlapped::status()
{
	DWORD transferredBytes;
	DWORD res = GetOverlappedResult(_handle, _overlapped, &transferredBytes, 0);
	if (res) {
		return ::datapath::error::Success;
	} else {
		DWORD err = GetLastError();
		switch (err) {
		case ERROR_PIPE_NOT_CONNECTED:
			return ::datapath::error::SocketClosed;
		case ERROR_IO_PENDING:
			return ::datapath::error::Success;
		default:
			return ::datapath::error::Failure;
		}
	}
}
