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

#include "waitable.hpp"
#include <assert.h>

extern "C" {
#include <Windows.h>
}

datapath::error datapath::waitable::wait(datapath::waitable* obj, std::chrono::nanoseconds duration)
{
	assert(obj != nullptr);

	HANDLE  handle  = (HANDLE)obj->get_waitable();
	int64_t timeout = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

	if (timeout < 0) {
		timeout = 0;
	} else if (timeout > std::numeric_limits<int32_t>::max()) {
		timeout = std::numeric_limits<int32_t>::max();
	}

	do {
		auto start = std::chrono::high_resolution_clock::now();

		DWORD result = WaitForSingleObjectEx(handle, DWORD(timeout), TRUE);
		switch (result) {
		case WAIT_OBJECT_0:
			obj->on_wait_success(datapath::error::Success);
			return datapath::error::Success;
		case WAIT_TIMEOUT:
			return datapath::error::TimedOut;
		case WAIT_ABANDONED:
			obj->on_wait_error(datapath::error::Closed);
			return datapath::error::Closed;
		case WAIT_IO_COMPLETION:
			duration = (std::chrono::high_resolution_clock::now() - start);
			timeout  -= std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
			if (timeout <= 0) {
				timeout = 0;
			}
			continue;
		default:
			return datapath::error::Failure;
		}
	} while (timeout >= 0);

	return datapath::error::TimedOut;
}

datapath::error datapath::waitable::wait(datapath::waitable** objs, size_t count, std::chrono::nanoseconds duration)
{
	assert(objs != nullptr);
	assert((count > 0) && (count <= MAXIMUM_WAIT_OBJECTS));

	int64_t timeout = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

	if (timeout < 0) {
		timeout = 0;
	} else if (timeout > std::numeric_limits<int32_t>::max()) {
		timeout = std::numeric_limits<int32_t>::max();
	}

	// Rebuild a valid obj+index translation list.
	std::vector<HANDLE> handles(count);
	std::vector<size_t> indexes(count);
	size_t              valid_handles = 0;
	for (size_t idx = 0; idx < count; idx++) {
		datapath::waitable* obj = objs[idx];
		if (obj) {
			handles[valid_handles] = reinterpret_cast<HANDLE>(obj->get_waitable());
			indexes[valid_handles] = idx;
			valid_handles++;
		}
	}

	do {
		auto start = std::chrono::high_resolution_clock::now();

		DWORD result = WaitForMultipleObjectsEx(handles.size(), handles.data(), TRUE, DWORD(timeout), TRUE);
		if ((result >= WAIT_OBJECT_0) && (result < (WAIT_OBJECT_0 + MAXIMUM_WAIT_OBJECTS))) {
			for (auto idx : indexes) {
				objs[idx]->on_wait_success(datapath::error::Success);
			}
			return datapath::error::Success;
		} else if ((result >= WAIT_ABANDONED_0) && (result < (WAIT_ABANDONED_0 + MAXIMUM_WAIT_OBJECTS))) {
			for (auto idx : indexes) {
				objs[idx]->on_wait_error(datapath::error::Closed);
			}
			return datapath::error::Closed;
		} else if (result == WAIT_TIMEOUT) {
			return datapath::error::TimedOut;
		} else if (result == WAIT_IO_COMPLETION) {
			duration = (std::chrono::high_resolution_clock::now() - start);
			timeout  -= std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
			if (timeout <= 0) {
				timeout = 0;
			}
			continue;
		} else {
			return datapath::error::Failure;
		}
	} while (timeout >= 0);

	return datapath::error::TimedOut;
}

datapath::error datapath::waitable::wait_any(datapath::waitable** objs, size_t count, size_t& index,
                                             std::chrono::nanoseconds duration)
{
	assert(objs != nullptr);
	assert((count > 0) && (count <= MAXIMUM_WAIT_OBJECTS));

	int64_t timeout = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

	if (timeout < 0) {
		timeout = 0;
	} else if (timeout > std::numeric_limits<int32_t>::max()) {
		timeout = std::numeric_limits<int32_t>::max();
	}

	// Rebuild a valid obj+index translation list.
	std::vector<HANDLE> handles(count);
	std::vector<size_t> indexes(count);
	size_t              valid_handles = 0;
	for (size_t idx = 0; idx < count; idx++) {
		datapath::waitable* obj = objs[idx];
		if (obj) {
			handles[valid_handles] = reinterpret_cast<HANDLE>(obj->get_waitable());
			indexes[valid_handles] = idx;
			valid_handles++;
		}
	}

	do {
		auto start = std::chrono::high_resolution_clock::now();

		DWORD result = WaitForMultipleObjectsEx(handles.size(), handles.data(), FALSE, DWORD(timeout), TRUE);
		if ((result >= WAIT_OBJECT_0) && (result < (WAIT_OBJECT_0 + MAXIMUM_WAIT_OBJECTS))) {
			index = indexes[result - WAIT_OBJECT_0];
			objs[index]->on_wait_success(datapath::error::Success);
			return datapath::error::Success;
		} else if ((result >= WAIT_ABANDONED_0) && (result < (WAIT_ABANDONED_0 + MAXIMUM_WAIT_OBJECTS))) {
			index = indexes[result - WAIT_OBJECT_0];
			objs[index]->on_wait_error(datapath::error::Closed);
			return datapath::error::Closed;
		} else if (result == WAIT_TIMEOUT) {
			return datapath::error::TimedOut;
		} else if (result == WAIT_IO_COMPLETION) {
			duration = (std::chrono::high_resolution_clock::now() - start);
			timeout  -= std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
			if (timeout <= 0) {
				timeout = 0;
			}
			continue;
		} else {
			return datapath::error::Failure;
		}
	} while (timeout >= 0);

	return datapath::error::TimedOut;
}
