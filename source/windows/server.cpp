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

#include "server.hpp"
#include "socket.hpp"
#include "utility.hpp"

// Buffer Size that Windows just ignores, for the most part.
#define WIN_BUFFER_SIZE 64 * 1024 * 1024
#define WIN_WAIT_TIME 100
#define WIN_BACKLOG_NUM 8

datapath::error datapath::windows::server::create(std::string path, datapath::permissions permissions,
                                                  size_t max_clients)
{
	// If old sockets are available, close them.
	this->close();

	// Apply options
	this->max_clients = max_clients;
	this->path        = path;

	// Generate Security Attributes.
	std::memset(&this->security_attributes, 0, sizeof(SECURITY_ATTRIBUTES));
	this->security_attributes.nLength              = sizeof(SECURITY_ATTRIBUTES);
	this->security_attributes.lpSecurityDescriptor = nullptr;
	this->security_attributes.bInheritHandle       = true;
	// TODO: Respect permissions.

	// Spawn x backlog connections
	// TODO: Add parameter for this.
	for (size_t n = 0; n < WIN_BACKLOG_NUM; n++) {
		HANDLE handle = _create_socket(path, n == 0);
		if (handle == INVALID_HANDLE_VALUE) {
			// Clean up again.
			this->close();
			return datapath::error::CriticalFailure;
		}

		{
			std::unique_lock<std::mutex> ul(this->lock);
			sockets.push_back(handle);
			waiting_sockets.push_back(handle);
		}
	}

	// Watcher Thread
	{
		std::unique_lock<std::mutex> ul(this->watcher.lock);
		this->watcher.shutdown = false;
		this->watcher.task     = std::thread(std::bind(&datapath::windows::server::_watcher, this));
	}

	is_created = true;
	return datapath::error::Success;
}

HANDLE datapath::windows::server::_create_socket(std::string path, bool initial)
{
	if (!datapath::windows::utility::make_pipe_path(path)) {
		return INVALID_HANDLE_VALUE;
	}
	std::wstring wpath = datapath::windows::utility::make_wide_string(path);

	DWORD file_flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED;
	if (initial) {
		file_flags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
	}

	DWORD pipe_flags = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;

	HANDLE handle = CreateNamedPipeW(wpath.c_str(), file_flags, pipe_flags, PIPE_UNLIMITED_INSTANCES,
	                                 WIN_BUFFER_SIZE, WIN_BUFFER_SIZE, WIN_WAIT_TIME, &this->security_attributes);
	return handle;
}

void datapath::windows::server::_watcher()
{
	std::map<HANDLE, std::shared_ptr<datapath::windows::overlapped>> ovmap;

	while (!this->watcher.shutdown) {
		// Wait for any overlapped objects.
		if (ovmap.size() > 0) {
			// No lock as we aren't touching any list or map yet.
			std::vector<datapath::waitable*> waits;
			waits.reserve(ovmap.size());

			for (auto kv : ovmap) {
				waits.push_back(&(*kv.second));
			}

			size_t          index = 0;
			datapath::error ec = datapath::waitable::wait_any(waits, index, std::chrono::milliseconds(0));
			if (ec != datapath::error::Success) {
				datapath::waitable::wait_any(waits, index, std::chrono::milliseconds(1));
			}
		}

		// Update list of overlappeds to track.
		{
			std::unique_lock<std::mutex> ul(this->lock);
			std::list<HANDLE>            to_kill;

			for (auto itr = this->waiting_sockets.begin(); itr != this->waiting_sockets.end(); itr++) {
				if (ovmap.count(*itr) == 0) {
					HANDLE handle = *itr;
					auto   ov     = std::make_shared<datapath::windows::overlapped>();
					ov->set_handle(handle);
					ov->set_data(this);
					ov->on_wait_success.add([this, &ovmap, &itr, &handle](datapath::error ec) {
						std::unique_lock<std::mutex> ul(this->lock);
						this->waiting_sockets.erase(itr);
						this->pending_sockets.push_back(handle);
						ovmap.erase(handle);
					});
					SetLastError(ERROR_SUCCESS);
					BOOL suc = ConnectNamedPipe(handle, ov->get_overlapped());
					if (suc) {
						ovmap.insert({handle, ov});
					} else {
						if (GetLastError() == ERROR_PIPE_CONNECTED) {
							to_kill.push_back(handle);
							this->pending_sockets.push_back(handle);
						} else {
							continue;
						}
					}
				}
			}
			for (auto hnd : to_kill) {
				this->waiting_sockets.remove(hnd);
			}
		}

		// Notify of pending sockets.
		{
			std::unique_lock<std::mutex> ul(this->lock);
			std::list<HANDLE>            to_kill;

			if (this->on_accept) {
				for (auto itr = this->pending_sockets.begin(); itr != this->pending_sockets.end();
				     itr++) {
					HANDLE handle = *itr;
					bool   accept = true;

					auto sock = std::make_shared<datapath::windows::socket>();
					sock->_connect(handle);

					auto isock = std::dynamic_pointer_cast<datapath::isocket>(sock);
					this->on_accept(accept, isock);

					if (accept) {
						to_kill.push_back(handle);
						this->active_sockets.insert({handle, sock});

						if ((this->waiting_sockets.size() + this->pending_sockets.size())
						    < WIN_BACKLOG_NUM) {
							if ((this->sockets.size() <= this->max_clients)
							    && (this->max_clients > 0)) {
								HANDLE handle = _create_socket(this->path, false);
								if (handle != INVALID_HANDLE_VALUE) {
									this->sockets.push_back(handle);
									this->waiting_sockets.push_back(handle);
								}
							}
						}
					} else {
						// Force close and return to waiting.
						sock->close();
						to_kill.push_back(handle);
						this->waiting_sockets.push_back(handle);
					}
				}
			}
			for (auto hnd : to_kill) {
				this->pending_sockets.remove(hnd);
			}
		}

		// Verify existing connections.
		{
			std::list<HANDLE>            to_kill;
			std::unique_lock<std::mutex> ul(this->lock);
			for (auto itr = this->active_sockets.begin(); itr != this->active_sockets.end(); itr++) {
				if (itr->second.expired()) {
					// Enforce backlog size
					if ((this->waiting_sockets.size() + this->pending_sockets.size())
					    < WIN_BACKLOG_NUM) {
						this->waiting_sockets.push_back(itr->first);
					} else {
						DisconnectNamedPipe(itr->first);
						CloseHandle(itr->first);
						to_kill.push_back(itr->first);
					}
					this->active_sockets.erase(itr);
					continue;
				}
				auto obj = itr->second.lock();
				if (!obj->good()) {
					// Enforce backlog size
					if ((this->waiting_sockets.size() + this->pending_sockets.size())
					    < WIN_BACKLOG_NUM) {
						this->waiting_sockets.push_back(itr->first);
					} else {
						DisconnectNamedPipe(itr->first);
						CloseHandle(itr->first);
						to_kill.push_back(itr->first);
					}
					this->active_sockets.erase(itr);
					continue;
				}
			}
			for (auto hnd : to_kill) {
				this->active_sockets.erase(hnd);
			}
		}

		if (ovmap.size() == 0) {
			// Just sleep 1ms to not use too much CPU.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

/*datapath::error datapath::windows::server::accept(std::shared_ptr<datapath::isocket>& socket)
{
	std::unique_lock<std::mutex> ul(this->lock);
	if (this->pending_sockets.size() == 0) {
		return datapath::error::Failure;
	}

	HANDLE handle = this->pending_sockets.front();
	this->pending_sockets.pop_front();

	if (!socket) {
		socket = std::dynamic_pointer_cast<datapath::isocket>(std::make_shared<datapath::windows::socket>());
	}
	std::shared_ptr<datapath::windows::socket> obj = std::dynamic_pointer_cast<datapath::windows::socket>(socket);

	obj->_connect(handle);

	// Stock up on backlog and total sockets.
	if ((this->waiting_sockets.size() + this->pending_sockets.size()) < WIN_BACKLOG_NUM) {
		if ((this->sockets.size() <= this->max_clients) && (this->max_clients > 0)) {
			HANDLE handle = _create_socket(this->path, false);
			if (handle != INVALID_HANDLE_VALUE) {
				this->sockets.push_back(handle);
				this->waiting_sockets.push_back(handle);
			}
		}
	}

	this->active_sockets.insert({handle, socket});
	return datapath::error::Success;
}*/

datapath::error datapath::windows::server::close()
{
	// Watcher Thread
	{
		std::unique_lock<std::mutex> ul(this->watcher.lock);
		this->watcher.shutdown = true;
		if (this->watcher.task.joinable()) {
			this->watcher.task.join();
		}
	}

	// Kill all sockets.
	std::unique_lock<std::mutex> ul(this->lock);
	for (HANDLE socket : sockets) {
		DisconnectNamedPipe(socket);
		CloseHandle(socket);
	}

	// Notify Sockets of being dead.

	// Clear all lists.
	sockets.clear();
	waiting_sockets.clear();
	pending_sockets.clear();
	active_sockets.clear();

	return datapath::error::Success;
}

datapath::error datapath::windows::server::host(std::shared_ptr<datapath::iserver>& server, std::string path,
                                                datapath::permissions permissions, size_t max_clients)
{
	if (!server) {
		server = std::dynamic_pointer_cast<datapath::iserver>(std::make_shared<datapath::windows::server>());
	}
	std::shared_ptr<datapath::windows::server> obj = std::dynamic_pointer_cast<datapath::windows::server>(server);

	return obj->create(path, permissions, max_clients);
}
