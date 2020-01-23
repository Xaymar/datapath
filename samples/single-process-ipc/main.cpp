/*
Sample for DataPath
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

#include <datapath/datapath.hpp>
#include <datapath/server.hpp>
#include <datapath/socket.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

constexpr const char* socket_path = "sample-simple-process-ipc";

void do_log(const char* format, ...)
{
	static std::mutex           lock;
	static std::vector<char>    buffer;
	std::lock_guard<std::mutex> lg(lock);

	va_list args;
	va_start(args, format);
	va_list args2;
	va_copy(args2, args);

	buffer.resize(std::vsnprintf(nullptr, 0, format, args));
	va_end(args);
	std::vsnprintf(buffer.data(), buffer.size(), format, args2);
	va_end(args2);

	std::cout.write(buffer.data(), buffer.size());
}

class server {
	std::shared_ptr<::datapath::server> _server;
	std::shared_ptr<::datapath::socket> _conn;

	std::list<std::thread> _threads;

	public:
	server()
	{
		_server = ::datapath::server::create();
		_server->set_path(socket_path);
		_server->events.connected +=
			std::bind(&::server::on_connected, this, std::placeholders::_1, std::placeholders::_2);
		_server->open();
		for (size_t idx = 0, edx = 4; idx < edx; idx++) {
			_threads.push_back(std::move(std::thread(std::bind(&::server::work, this))));
		}
		do_log("[SERVER] Listening on '%s'...\n", socket_path);
	}
	~server()
	{
		do_log("[SERVER] Stopping...\n");
		_server->close();
		for (auto& thread : _threads) {
			thread.join();
		}
		do_log("[SERVER] Stopped.\n");
	}

	void work()
	{
		do_log("[SERVER/THREAD] Working...\n");
		while (_server->is_open()) {
			_server->work();
		}
		do_log("[SERVER/THREAD] Work done.\n");
	}

	void on_connected(bool& allow, std::shared_ptr<::datapath::socket> socket)
	{
		allow = true;
		_conn = socket;
		_conn->events.closed +=
			std::bind(&::server::on_disconnected, this, std::placeholders::_1, std::placeholders::_2);
		_conn->read(std::bind(&::server::on_read_completed, this, std::placeholders::_1, std::placeholders::_2,
							  std::placeholders::_3, std::placeholders::_4),
					nullptr);
		do_log("[SERVER] New client connected!\n");
	}

	void on_disconnected(::datapath::error, std::shared_ptr<::datapath::socket>)
	{
		do_log("[SERVER] Client left us.\n");
	}

	void on_read_completed(std::shared_ptr<::datapath::socket>, ::datapath::error, const ::datapath::io_data_t& data,
						   ::datapath::io_callback_data_t)
	{
		do_log("[SERVER] Client sent %llu bytes, with content: %.*s\n", data.size(), data.size(), data.data());
		_conn->read(std::bind(&::server::on_read_completed, this, std::placeholders::_1, std::placeholders::_2,
							  std::placeholders::_3, std::placeholders::_4),
					nullptr);
	}
};

class client {
	std::shared_ptr<::datapath::socket> _client;
	std::list<std::thread>              _threads;
	std::vector<char>                   _data;
	std::atomic_bool                    _stop = false;

	public:
	client()
	{
		const char* str = "Hello World";
		_data.resize(strlen(str));
		memcpy(_data.data(), str, _data.size());

		do_log("[CLIENT] Connecting to '%s'...\n", socket_path);
		_client = ::datapath::socket::create();
		_client->set_path(socket_path);
		_client->events.opened +=
			std::bind(&::client::on_connected, this, std::placeholders::_1, std::placeholders::_2);

		for (size_t idx = 0, edx = 4; idx < edx; idx++) {
			_threads.push_back(std::move(std::thread(std::bind(&::client::work, this))));
		}

		_client->open();

		_client->write(reinterpret_cast<uint8_t*>(_data.data()), _data.size(),
					   std::bind(&::client::on_write_completed, this, std::placeholders::_1, std::placeholders::_2,
								 std::placeholders::_3, std::placeholders::_4),
					   nullptr);
	}
	~client()
	{
		do_log("[CLIENT] Stopping...\n");
		_client->close();
		_stop = true;
		for (auto& thread : _threads) {
			thread.join();
		}
		do_log("[CLIENT] Stopped.\n");
	}

	void work()
	{
		do_log("[CLIENT/THREAD] Working...\n");
		while (!_stop) {
			_client->work();
		}
		do_log("[CLIENT/THREAD] Work done.\n");
	}

	void on_connected(::datapath::error, std::shared_ptr<::datapath::socket>)
	{
		do_log("[CLIENT] We are in!\n");
		for (size_t idx = 0; idx < 100; idx++) {
			_client->write(reinterpret_cast<uint8_t*>(_data.data()), _data.size(),
						   std::bind(&::client::on_write_completed, this, std::placeholders::_1, std::placeholders::_2,
									 std::placeholders::_3, std::placeholders::_4),
						   nullptr);
		}
	}

	void on_write_completed(std::shared_ptr<::datapath::socket>, ::datapath::error, const ::datapath::io_data_t& data,
							::datapath::io_callback_data_t)
	{
		do_log("[CLIENT] Sent %llu bytes with content: %.*s\n", data.size(), data.size(), data.data());
	}
};

int main(int argc, const char* argv[])
{
	try {
		auto my_server = std::make_shared<::server>();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		try {
			auto my_client = std::make_shared<::client>();
			std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		} catch (std::exception const& ex) {
			std::cerr << ex.what() << std::endl;
			return 1;
		}
	} catch (std::exception const& ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}
	return 0;
}
