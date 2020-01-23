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

#include "windows-server.hpp"
#include "socket.hpp"
#include "windows-server-socket.hpp"
#include "windows-utility.hpp"

#include <algorithm>
#include <thread>

const std::size_t BACKLOG = 8;

// Windows implementation of 'server.hpp:L32'.
std::shared_ptr<::datapath::server> datapath::server::create()
{
	return std::make_shared<::datapath::windows::server>();
}

datapath::windows::server::server() : _opened(false), _path(), _lock(), _sockets(), _sockets_free(), _iocp() {}

datapath::windows::server::~server()
{
	close();
}

void datapath::windows::server::set_path(std::string path)
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

void datapath::windows::server::open()
{
	try {
		// Close the server just to be safe.
		close();

		// Guard against multiple invocations.
		std::lock_guard<std::mutex> lg(_lock);

		// Create a core IO Completion Port.
		HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, reinterpret_cast<ULONG_PTR>(this), 0);
		if (iocp == INVALID_HANDLE_VALUE)
			throw std::runtime_error("Failed to create IOCompletionPort.");
		_iocp = std::shared_ptr<void>(iocp, ::datapath::windows::utility::shared_ptr_handle_deleter);

		// Spawn a backlog of sockets.
		for (std::size_t idx = 0; idx < BACKLOG; idx++) {
			auto socket = create_socket();
			_sockets_free++;
			_sockets.push_back(socket);
		}

		// Change state.
		_opened.store(true);
	} catch (std::exception const& ex) {
		// If there was any problem during all of this, close the server,
		//  and throw the exception to the parent caller.
		close();
		throw ex;
	}
}

void datapath::windows::server::close()
{
	// Guard against multiple invocations.
	std::lock_guard<std::mutex> lg(_lock);

	// Update state.
	_opened.store(false);

	// Disconnect all clients
	for (auto socket : _sockets) {
		socket->close();
	}
	_sockets.clear();

	// Kill Workers
	for (std::size_t idx = 0, edx = _worker_count.load(); idx < edx; idx++) {
		PostQueuedCompletionStatus(_iocp.get(), NULL, NULL, NULL);
	}

	// Close handles.
	_iocp.reset();
}

bool datapath::windows::server::is_open()
{
	return _opened.load();
}

void datapath::windows::server::work(std::chrono::milliseconds time_limit)
{
	// Thread local state.
	std::shared_ptr<void> iocp;
	auto                  limit                 = time_limit.count();
	DWORD                 w_time_limit          = (limit > 2147483648) ? INFINITE : static_cast<DWORD>(limit);
	DWORD                 num_bytes_transferred = 0;
	ULONG_PTR             ptr                   = 0;
	LPOVERLAPPED          overlapped            = 0;
	DWORD                 error_code            = ERROR_SUCCESS;
	windows::overlapped*  ovp                   = nullptr;

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
		ovp = windows::overlapped::from_overlapped(overlapped);
		ovp->callback(static_cast<size_t>(num_bytes_transferred), reinterpret_cast<void*>(ptr));
	}

	// Request failed, check error code.
	// ToDo: Deal with other error codes?
	error_code = GetLastError();
	return;
}

std::shared_ptr<datapath::windows::server_socket> datapath::windows::server::create_socket()
{
	auto socket = std::make_shared<::datapath::windows::server_socket>(shared_from_this(), _sockets.size() == 0);
	socket->internal_events.opened +=
		std::bind(&::datapath::windows::server::on_socket_opened, this, std::placeholders::_1, std::placeholders::_2);
	socket->internal_events.closed +=
		std::bind(&::datapath::windows::server::on_socket_closed, this, std::placeholders::_1, std::placeholders::_2);
	socket->open();
	return socket;
}

void datapath::windows::server::on_socket_opened(::datapath::error error, std::shared_ptr<::datapath::socket> socket)
{
	// ToDo: Deal with other error codes.
	if (error != ::datapath::error::Success)
		return;

	// Check if the connection should be allowed.
	bool allow = false;
	events.connected(allow, socket);

	if (allow) {
		// If the connection was allowed, reduce the free socket count.
		_sockets_free--;

		// We're on or over the socket limit, so don't bother filling up.
		if (_sockets_free >= BACKLOG)
			return;

		{
			std::lock_guard<std::mutex> lg(_lock);
			_sockets.push_back(create_socket());
		}
		_sockets_free++;
	} else {
		// If the connection was not allowed, reset and wait for new connection.
		socket->close();
	}
}

void datapath::windows::server::on_socket_closed(::datapath::error error, std::shared_ptr<::datapath::socket> socket)
{
	if (_sockets_free < BACKLOG) {
		socket->open();
		_sockets_free++;
	} else {
		std::lock_guard<std::mutex> lg(_lock);
		_sockets.remove(std::dynamic_pointer_cast<::datapath::windows::server_socket>(socket));
	}
}

std::wstring& datapath::windows::server::path()
{
	return _path;
}

std::shared_ptr<void> datapath::windows::server::iocp()
{
	return _iocp;
}
