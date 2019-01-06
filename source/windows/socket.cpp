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

#include "socket.hpp"
#include <cinttypes>
#include "task.hpp"
#include "utility.hpp"

#define SIZE_ELEMENT uint32_t

void datapath::windows::socket::_connect(HANDLE handle)
{
	this->socket_handle = handle;
	if (handle != INVALID_HANDLE_VALUE) {
		this->is_connected = true;

		{
			std::unique_lock<std::mutex> ul(this->watcher.lock);
			this->watcher.shutdown = false;
			this->watcher.task     = std::thread(std::bind(&datapath::windows::socket::_watcher, this));
		}
	}
}

void datapath::windows::socket::_disconnect()
{
	if (this->on_close) {
		this->on_close();
	}

	{
		{
			std::unique_lock<std::mutex> ul(this->watcher.lock);
			this->watcher.shutdown = true;
		}
		if (this->watcher.task.joinable()) {
			this->watcher.task.join();
		}
	}

	this->is_connected = false;
}

void datapath::windows::socket::_watcher()
{
	enum class readstate { Unknown, Header, Content } state = readstate::Unknown;

	std::vector<char> read_buffer;

	std::shared_ptr<datapath::windows::overlapped> read_header_ov =
	    std::make_shared<datapath::windows::overlapped>();
	std::shared_ptr<datapath::windows::overlapped> read_content_ov =
	    std::make_shared<datapath::windows::overlapped>();
	std::shared_ptr<datapath::windows::overlapped> waitable;

	read_header_ov->on_wait_error.add([&state, &waitable](datapath::error ec) {
		// There was an error waiting on the header.
		state = readstate::Unknown;
		waitable.reset();
	});
	read_header_ov->on_wait_success.add(
	    [this, &read_buffer, &read_content_ov, &state, &waitable](datapath::error ec) {
		    read_content_ov->set_handle(this->socket_handle);
		    read_content_ov->set_data(this);

		    // ToDo: Add optional message size limit, messages above this size kill the connection for attempting DoS.
		    size_t msg_size = reinterpret_cast<SIZE_ELEMENT&>(read_buffer[0]);
		    read_buffer.resize(msg_size);

		    // Read content.
		    if (ReadFileEx(this->socket_handle, read_buffer.data(), DWORD(read_buffer.size()),
		                   read_content_ov->get_overlapped(), NULL)) {
			    state    = readstate::Content;
			    waitable = read_content_ov;
		    } else {
			    state = readstate::Unknown;
			    waitable.reset();
		    }
	    });
	read_content_ov->on_wait_error.add([&state, &waitable](datapath::error ec) {
		// There was an error waiting on the content.
		state = readstate::Unknown;
		waitable.reset();
	});
	read_content_ov->on_wait_success.add([this, &read_buffer, &state, &waitable](datapath::error ec) {
		// We have content!
		if (this->on_message) {
			this->on_message(read_buffer);
			state = readstate::Unknown;
		} else {
			// We're buffering the message in read_buffer until there is a hook to on_message.
		}
		waitable.reset();
	});

	while (!this->watcher.shutdown) {
		if (this->socket_handle == INVALID_HANDLE_VALUE) {
			break;
		}
		if (!this->is_connected) {
			break;
		}

		if (state == readstate::Unknown) {
			// Read the header of the next message.
			// The header simply contains the length of the message.
			// ToDo: Figure out if Message transfer/read mode and WaitCommEvent work together.

			read_header_ov->set_handle(this->socket_handle);
			read_header_ov->set_data(this);
			read_buffer.resize(sizeof(SIZE_ELEMENT));

			// Read content.
			if (ReadFileEx(this->socket_handle, read_buffer.data(), DWORD(read_buffer.size()),
			               read_header_ov->get_overlapped(),
			               &datapath::windows::utility::def_io_completion_routine)) {
				state    = readstate::Header;
				waitable = read_header_ov;

			} else {
				state = readstate::Unknown;
				waitable.reset();
			}
		} else if (state == readstate::Header) {
			// This logic is in the on_wait_success handler.
		} else if (state == readstate::Content) {
			// This logic is in the on_wait_success handler, and continued here.
			if (!waitable) {
				// We currently have a message buffered, but there was no handler last time we checked.
				if (this->on_message) {
					this->on_message(read_buffer);
					state = readstate::Unknown;
				}
			}
		}

		if (!waitable) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} else {
			datapath::error err = waitable->wait(std::chrono::milliseconds(1));
			if (err == datapath::error::Closed) {
				_disconnect();
				continue;
			}
		}
	}
}

datapath::windows::socket::socket() : is_connected(false), socket_handle(INVALID_HANDLE_VALUE) {}

datapath::windows::socket::~socket()
{
	close();
}

bool datapath::windows::socket::good()
{
	return this->is_connected;
}

datapath::error datapath::windows::socket::close()
{
	if (this->is_connected) {
		DisconnectNamedPipe(this->socket_handle);
		_disconnect();
		return datapath::error::Success;
	}
	return datapath::error::Closed;
}

datapath::error datapath::windows::socket::write(std::shared_ptr<datapath::itask>& task, const std::vector<char>& data)
{
	if (!task) {
		task = std::dynamic_pointer_cast<datapath::itask>(std::make_shared<datapath::windows::task>());
	}
	std::shared_ptr<datapath::windows::task>       obj = std::dynamic_pointer_cast<datapath::windows::task>(task);
	std::shared_ptr<datapath::windows::overlapped> ov  = std::make_shared<datapath::windows::overlapped>();

	obj->_assign(data, ov);

	BOOL suc = WriteFileEx(socket_handle, obj->data().data(), DWORD(obj->data().size()), ov->get_overlapped(),
	                       &datapath::windows::utility::def_io_completion_routine);
	if (suc) {
		return datapath::error::Success;
	} else {
		return datapath::error::Failure;
	}
}

datapath::error datapath::windows::socket::connect(std::shared_ptr<datapath::isocket>& socket, std::string path)
{
	if (!datapath::windows::utility::make_pipe_path(path)) {
		return datapath::error::InvalidPath;
	}
	std::wstring wpath = datapath::windows::utility::make_wide_string(path);

	SetLastError(ERROR_SUCCESS);
	HANDLE handle = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	                            FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
	if ((handle == INVALID_HANDLE_VALUE) || (GetLastError() != ERROR_SUCCESS)) {
		return datapath::error::Failure;
	}

	DWORD pipe_read_mode = PIPE_WAIT | PIPE_READMODE_BYTE;

	SetLastError(ERROR_SUCCESS);
	if (!SetNamedPipeHandleState(handle, &pipe_read_mode, NULL, NULL)) {
		// ToDo. This doesn't actually affect us as the default mode is the one we're setting
	}

	if (!socket) {
		socket = std::dynamic_pointer_cast<datapath::isocket>(std::make_shared<datapath::windows::socket>());
	}
	std::shared_ptr<datapath::windows::socket> obj = std::dynamic_pointer_cast<datapath::windows::socket>(socket);

	obj->_connect(handle);

	return datapath::error::Success;
}
