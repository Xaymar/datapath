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

#include "windows-server-socket.hpp"
#include "windows-utility.hpp"

// Server sockets behave differently from client sockets:
// 1. The Named Pipe is immediately created in the constructor.
// 2. The Named Pipe is destroyed only in the destructor.
// 3. It does not have its own threads to work with (shares threads with Server).
// 4. The opened callback is handled by the Server.
// 5. Open/Close are used to reset the socket.
//
// Identical Stuff
// - Read/Write must be queued.
// - Packets are
//   0..3	4-byte unsigned packet size
//   4...   Packet Data
//
// Queue Behavior
// - All reads/writes are inserted to the queue.
// - Only the front of the queue can be worked on (for stability reasons).
//

#define DATAPATH_PIPE_FLAGS PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED
#define DATAPATH_PIPE_MODE PIPE_TYPE_BYTE | PIPE_READMODE_BYTE
#define BUFFER_SIZE 64 * 1024
#define WAIT_TIME_OUT 10

datapath::windows::server_socket::server_socket(std::shared_ptr<::datapath::windows::server> parent, bool initial)
	: socket(), _lock(), _opened(false), _iocp(), _handle(), _ov_open(), _parent(parent)
{
	// Create an asynchronous duplex byte name pipe.
	DWORD flags = DATAPATH_PIPE_FLAGS;
	if (initial) // If this is supposed to be the first instance, set the flag for it.
		flags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
	_handle =
		std::shared_ptr<void>(CreateNamedPipeW(parent->path().c_str(), flags, DATAPATH_PIPE_MODE,
											   PIPE_UNLIMITED_INSTANCES, BUFFER_SIZE, BUFFER_SIZE, WAIT_TIME_OUT, NULL),
							  utility::shared_ptr_handle_deleter);

	if (_handle.get() == INVALID_HANDLE_VALUE) {
		throw std::runtime_error("Failed to create socket.");
	}

	{ // Create IOCP.
		_iocp         = parent->iocp();
		HANDLE handle = CreateIoCompletionPort(_handle.get(), _iocp.get(), reinterpret_cast<ULONG_PTR>(this), 0);
		if ((handle != _iocp.get()) || (handle == INVALID_HANDLE_VALUE)) {
			CloseHandle(handle);
			throw std::runtime_error("Failed to IOCompletionPort.");
		}
	}

	// Set up OVERLAPPED.
	_ov_open.set_handle(_handle.get());
	_ov_open.set_callback(std::bind(&::datapath::windows::server_socket::on_open, this, std::placeholders::_1,
									std::placeholders::_2, std::placeholders::_3));
	_ov_read.set_handle(_handle.get());
	_ov_write.set_handle(_handle.get());
}

datapath::windows::server_socket::~server_socket()
{
	close();
}

void datapath::windows::server_socket::set_path(std::string path)
{
	throw std::runtime_error("Operation not supported.");
}

void datapath::windows::server_socket::open()
{
	try {
		// Close the server just to be safe.
		close();

		// Guard against multiple invocations.
		std::lock_guard<std::mutex> lg(_lock);

		// Enqueue an asynchronous attempt at connecting.
		SetLastError(ERROR_SUCCESS);
		ConnectNamedPipe(_handle.get(), _ov_open.get_overlapped());
		switch (GetLastError()) {
		case ERROR_SUCCESS:        // Client was waiting for us.
		case ERROR_PIPE_CONNECTED: // Client was waiting for us.
			// Occasionally ConnectNamedPipe can instantly finish, for example if a client has connected in the last
			// tick, but we haven't made a call to ConnectNamedPipe in that time.
			//on_open(_ov_open, 0, nullptr);
			return;
		case ERROR_IO_PENDING: // Waiting for client to connect.
			return;
		case ERROR_PIPE_LISTENING: // Client is already connected.
			return;                // Technically not a valid return code.
		default:
			throw std::runtime_error("TODO");
		}
	} catch (std::exception const& ex) {
		// If there was any problem during all of this, close the server,
		//  and throw the exception to the parent caller.
		close();
		throw ex;
	}
}

void datapath::windows::server_socket::close()
{
	// Guard against multiple invocations.
	std::lock_guard<std::mutex> lg(_lock);

	// If the socket is not yet opened, but has a pending connect operation, cancel it.
	SetLastError(ERROR_SUCCESS);
	if (!_opened) {
		CancelIoEx(_handle.get(), _ov_open.get_overlapped());
		DWORD res = DisconnectNamedPipe(_handle.get());
		if (res == 0) {
			DWORD ec = GetLastError();
		}
	} else {
		// If it is open, disconnect the client.
		DWORD res = DisconnectNamedPipe(_handle.get());
		_opened.store(false);

		if (res == 0) {
			DWORD ec = GetLastError();
			switch (ec) {
			case ERROR_SUCCESS:
				return;
			case ERROR_INVALID_HANDLE: // Something messed with the handle, so we can't use it anymore.
				throw std::runtime_error("Invalid error returned from system call.");
			default:
				throw std::runtime_error("Generic failure.");
			}
		}

		/* TODO: Should manually calling close actually call callbacks? Doesn't really make any sense.
		{
			auto status = ::datapath::error::SocketClosed;
			auto self   = shared_from_this();
			events.closed(status, self);
			internal_events.closed(status, self);
		}
		*/
	}
}

bool datapath::windows::server_socket::is_open()
{
	return _opened;
}

void datapath::windows::server_socket::work(std::chrono::milliseconds time_limit)
{
	return;
}

void datapath::windows::server_socket::on_open(::datapath::windows::overlapped& ov, std::size_t size, void* ptr)
{
	auto status = ov.status();
	auto self   = shared_from_this();

	if (status == error::Success) {
		_opened.store(true);
	} else {
		_opened.store(false);
	}

	internal_events.opened(status, self);
	events.opened(status, self);
}
