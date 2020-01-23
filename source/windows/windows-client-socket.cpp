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

#include "windows-client-socket.hpp"
#include "windows-utility.hpp"

::std::shared_ptr<::datapath::socket> datapath::socket::create()
{
	return std::make_shared<::datapath::windows::client_socket>();
}

datapath::windows::client_socket::client_socket()
	: socket(), _lock(), _opened(false), _path(), _worker_count(0), _iocp(), _handle()
{
	// Create IOCP
	HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, reinterpret_cast<ULONG_PTR>(this), 0);
	if (iocp == INVALID_HANDLE_VALUE)
		throw std::runtime_error("Failed to create IOCompletionPort.");
	_iocp = std::shared_ptr<void>(iocp, ::datapath::windows::utility::shared_ptr_handle_deleter);
}

datapath::windows::client_socket::~client_socket()
{
	close();
}

void datapath::windows::client_socket::set_path(std::string path)
{
	std::lock_guard<std::mutex> lg(_lock);

	if (_opened.load())
		throw std::runtime_error("TODO"); // ToDo: Better exceptions.

	// Create a proper path for Windows.
	if (!::datapath::windows::utility::make_pipe_path(path))
		throw std::runtime_error("TODO"); // ToDo: Better exceptions.

	// Store the new path.
	_path = utility::make_wide_string(path);
}

void datapath::windows::client_socket::open()
{
	try {
		// Close the server just to be safe.
		close();

		// Guard against multiple invocations.
		std::lock_guard<std::mutex> lg(_lock);

		// Create handle
		SetLastError(ERROR_SUCCESS);
		HANDLE handle = CreateFileW(_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
									FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
		if (handle == INVALID_HANDLE_VALUE) {
			throw std::runtime_error("TODO");
		}
		_handle = std::shared_ptr<void>(handle, ::datapath::windows::utility::shared_ptr_handle_deleter);

		// Create IOCP
		HANDLE iocp = CreateIoCompletionPort(handle, _iocp.get(), reinterpret_cast<ULONG_PTR>(this), 0);
		if (iocp == INVALID_HANDLE_VALUE)
			throw std::runtime_error("Failed to create IOCompletionPort.");
		if (iocp != _iocp.get()) {
			CloseHandle(iocp);
			throw std::runtime_error("Failed to link IOCompletionPort.");
		}

		// Update the pipe state, just to be safe.
		DWORD mode = PIPE_READMODE_BYTE;
		SetNamedPipeHandleState(_handle.get(), &mode, NULL, NULL);

		// Update the overlapped objects to the new handle.
		_ov_read.set_handle(_handle.get());
		_ov_write.set_handle(_handle.get());

		// Update state.
		_opened.store(true);
	} catch (std::exception const& ex) {
		// If there was any problem during all of this, close the server,
		//  and throw the exception to the parent caller.
		close();
		throw ex;
	}
}

void datapath::windows::client_socket::close()
{
	// Guard against multiple invocations.
	std::lock_guard<std::mutex> lg(_lock);

	// Update state.
	_opened.store(false);

	// Kill Workers
	for (std::size_t idx = 0, edx = _worker_count.load(); idx < edx; idx++) {
		PostQueuedCompletionStatus(_iocp.get(), NULL, NULL, NULL);
	}

	// Close handles.
	_iocp.reset();
	_handle.reset();
}

bool datapath::windows::client_socket::is_open()
{
	return _opened.load();
}

void datapath::windows::client_socket::work(std::chrono::milliseconds time_limit)
{
	// Thread local state.
	std::shared_ptr<void> iocp;
	auto                  limit                 = time_limit.count();
	DWORD                 w_time_limit          = (limit > 2147483648) ? INFINITE : static_cast<DWORD>(limit);
	DWORD                 num_bytes_transferred = 0;
	ULONG_PTR             ptr                   = 0;
	LPOVERLAPPED          overlapped            = 0;
	DWORD                 error_code            = ERROR_SUCCESS;

	{ // Copy necessary state.
		std::lock_guard<std::mutex> lg(_lock);
		if (!_opened.load()) {
			return;
		}

		iocp = _iocp;
	}

	// Try and get IOCP status.
	_worker_count++;
	DWORD status = GetQueuedCompletionStatus(iocp.get(), &num_bytes_transferred, &ptr, &overlapped, w_time_limit);
	_worker_count--;

	// Check if we suceeded.
	if (status == TRUE) {
		// Stop request or invalid status.
		if (!overlapped) {
			return;
		}

		// The OVERLAPPED object we will get here is actually always an instance
		//  of datapath::windows::overlapped. This allows us to go from it back
		//  to a proper object, and call the necessary callback, easily allowing
		//  new functionality to be added later.
		//
		// Note that this is technically very unsafe, however there is no other
		//  way to actually do this without writing a new Operating System.
		windows::overlapped* ovp = windows::overlapped::from_overlapped(overlapped);
		ovp->callback(static_cast<size_t>(num_bytes_transferred), reinterpret_cast<void*>(ptr));
	}

	// Request failed, check error code.
	// ToDo: Deal with other error codes?
	error_code = GetLastError();
	return;
}
