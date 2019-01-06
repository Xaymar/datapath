/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019  Michael Fabian Dirks <info@xaymar.com>

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

#include "overlapped.hpp"

datapath::windows::overlapped::overlapped() : overlapped_ptr(nullptr), data(nullptr)
{
	size_t memory_size = sizeof(OVERLAPPED) + sizeof(void*);
	buffer.resize(memory_size);

	reinterpret_cast<void*&>(buffer[sizeof(OVERLAPPED)]) = this;
	handle                                               = INVALID_HANDLE_VALUE;

	// Initialize OVERLAPPED
	overlapped_ptr = &reinterpret_cast<OVERLAPPED&>(buffer[0]);
	memset(overlapped_ptr, 0, sizeof(OVERLAPPED));
	overlapped_ptr->hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
}

datapath::windows::overlapped::~overlapped()
{
	cancel();
	buffer.clear();
}

OVERLAPPED* datapath::windows::overlapped::get_overlapped()
{
	return this->overlapped_ptr;
}

HANDLE datapath::windows::overlapped::get_handle()
{
	return this->handle;
}

void datapath::windows::overlapped::set_handle(HANDLE handle)
{
	this->handle = handle;
}

void* datapath::windows::overlapped::get_data()
{
	return this->data;
}

void datapath::windows::overlapped::set_data(void* data)
{
	this->data = data;
}

void datapath::windows::overlapped::cancel()
{
	if (overlapped_ptr) {
		CancelIoEx(handle, overlapped_ptr);
		ResetEvent(overlapped_ptr->hEvent);
		CloseHandle(overlapped_ptr->hEvent);
	}
}

bool datapath::windows::overlapped::is_completed()
{
	if (overlapped_ptr) {
		return HasOverlappedIoCompleted(overlapped_ptr);
	}
	return false;
}

void datapath::windows::overlapped::reset()
{
	cancel();
	if (overlapped_ptr) {
		overlapped_ptr->Internal     = 0;
		overlapped_ptr->InternalHigh = 0;
		overlapped_ptr->Offset       = 0;
		overlapped_ptr->OffsetHigh   = 0;
		overlapped_ptr->Pointer      = 0;
	}
}

void* datapath::windows::overlapped::get_waitable()
{
	return reinterpret_cast<void*>(overlapped_ptr->hEvent);
}
