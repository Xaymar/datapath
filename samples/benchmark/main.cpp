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

#include <cinttypes>
#include <cstdarg>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "datapath.hpp"
#include "measurer.hpp"

static auto log_time = std::chrono::high_resolution_clock::now();

#define COUNT 10000

void log(std::string format, ...)
{
	// Time
	uint32_t hour, minute, second;
	uint64_t nanosecond;

	{
		auto log_now = std::chrono::high_resolution_clock::now();

		nanosecond = (log_now - log_time).count();
		second     = nanosecond / 1000000000;
		nanosecond %= 1000000000;
		minute = second / 60;
		second %= 60;
		hour = minute / 60;
		minute %= 60;
	}

	// Message
	std::vector<char> msg;
	{
		va_list args;
		va_start(args, format);
		msg.resize(vsnprintf(nullptr, 0, format.c_str(), args) + 1);
		vsnprintf(msg.data(), msg.size(), format.c_str(), args);
		va_end(args);
	}

	printf("[%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32 ".%09" PRIu64 "] %.*s\n", hour, minute, second, nanosecond,
	       (int)msg.size(), msg.data());
}

// For simplicity, client and server will be isolated.
class server {
	std::shared_ptr<datapath::iserver> dp;

	struct client {
		std::shared_ptr<datapath::isocket> dp;
		server*                            parent;

		std::mutex                                  task_lock;
		std::list<std::shared_ptr<datapath::itask>> tasks;

		struct {
			std::thread task;
			bool        shutdown;
		} watcher;

		void _watcher()
		{
			std::vector<datapath::waitable*> waits;

			while (!this->watcher.shutdown) {
				size_t wait_cnt = 0;

				{
					std::unique_lock<std::mutex> ul(this->task_lock);
					wait_cnt = this->tasks.size();
				}

				if (wait_cnt == 0) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				} else {
					{
						std::unique_lock<std::mutex> ul(this->task_lock);
						waits.resize(0);
						waits.reserve(this->tasks.size());
						for (auto task : this->tasks) {
							waits.push_back(&(*task));
							if (waits.size() >= 64)
								break;
						}
					}

					size_t          index = 0;
					datapath::error ec =
					    datapath::waitable::wait_any(waits, index, std::chrono::milliseconds(0));
					if (ec != datapath::error::Success) {
						ec = datapath::waitable::wait_any(waits, index,
						                                  std::chrono::milliseconds(1));
					}

					if (ec == datapath::error::Success) {
						std::unique_lock<std::mutex> ul(this->task_lock);
						std::list<std::shared_ptr<datapath::itask>>::iterator task;
						for (auto itr = this->tasks.begin(); itr != this->tasks.end(); itr++) {
							if (&*(*itr) == waits[index]) {
								task = itr;
								break;
							}
						}
						this->tasks.erase(task);
					}
					waits.resize(0);
				}
			}
		}

		void handle_close()
		{
			//std::unique_lock<std::mutex> ul(this->parent->socket_lock);
			//this->parent->sockets.erase(dp);
		}

		void handle_message(const std::vector<char>& data)
		{
			std::shared_ptr<datapath::itask> task;
			datapath::error                  ec = dp->write(task, data);
			if (ec == datapath::error::Success) {
				std::unique_lock<std::mutex> ul(this->task_lock);
				tasks.push_back(task);
			}
		}

		public:
		client(server* parent, std::shared_ptr<datapath::isocket> socket)
		{
			this->parent = parent;
			dp           = socket;
			socket->on_close.add(std::bind(&server::client::handle_close, this));
			socket->on_message.add(std::bind(&server::client::handle_message, this, std::placeholders::_1));

			this->watcher.shutdown = false;
			this->watcher.task     = std::thread(std::bind(&server::client::_watcher, this));
		}

		~client()
		{
			this->watcher.shutdown = true;
			if (this->watcher.task.joinable()) {
				this->watcher.task.join();
			}

			dp->close();
		};
	};

	std::mutex                                                                    socket_lock;
	std::map<std::shared_ptr<datapath::isocket>, std::shared_ptr<server::client>> sockets;

	private:
	void handle_accept(bool& should_accept, std::shared_ptr<datapath::isocket> socket)
	{
		if (!socket->good()) {
			should_accept = false;
			return;
		}

		{
			std::unique_lock<std::mutex> ul(this->socket_lock);
			this->sockets.insert({socket, std::make_shared<server::client>(this, socket)});
		}
	}

	public:
	server(std::string path)
	{
		datapath::error ec = datapath::host(dp, path,
		                                    datapath::permissions::User | datapath::permissions::Group
		                                        | datapath::permissions::World);
		if (ec != datapath::error::Success) {
			throw std::exception();
		}

		dp->on_accept.add(
		    std::bind(&server::handle_accept, this, std::placeholders::_1, std::placeholders::_2));
	}

	~server()
	{
		{
			std::unique_lock<std::mutex> ul(this->socket_lock);
			this->sockets.clear();
		}
		if (dp) {
			dp->close();
		}
	}
};

class client {
	std::shared_ptr<datapath::isocket> dp;
	measurer                           ms;
	std::vector<char>                  write_buf;

	uint64_t cnt      = 0;
	uint64_t send_cnt = 0;
	bool     can_send_msg = true;

	struct {
		std::thread task;
		bool        shutdown;
	} watcher;

	private:
	void _watcher()
	{
		std::shared_ptr<datapath::itask> task;

		while (!this->watcher.shutdown) {
			if (send_cnt >= COUNT) {
				break;
			}

			auto start = std::chrono::high_resolution_clock::now();
			if (!task && send_cnt < COUNT && can_send_msg) {
				write_buf.resize(sizeof(uint64_t));
				reinterpret_cast<uint64_t&>(write_buf[0]) =
				    std::chrono::high_resolution_clock::now().time_since_epoch().count();
				datapath::error ec = dp->write(task, write_buf);
				send_cnt++;
				can_send_msg = false;
			}

			if (task) {
				datapath::error ec = task->wait(std::chrono::milliseconds(0));
				if (ec != datapath::error::Success) {
					ec = task->wait(std::chrono::milliseconds(100));
				}

				if (ec == datapath::error::Success) {
					//log("Sent Message, error code %lu...", uint32_t(ec));
					task.reset();
				} else if (ec != datapath::error::TimedOut) {
					log("Failed, error code %lu...", uint32_t(ec));
					task.reset();
				}
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	}

	void handle_message(const std::vector<char>& data)
	{
		can_send_msg = true;
		// Message is a timestamp of when it was sent.
		uint64_t cur = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		uint64_t msg = reinterpret_cast<const uint64_t&>(data[0]);
		uint64_t dlt = cur - msg;

		ms.track(std::chrono::nanoseconds(dlt));

		cnt++;
		if (cnt % 1000 == 0) {
			log("%llu messages.", cnt);
		}
		if (cnt >= COUNT) {
			// Print stats.
			log("Buffer Size: %llu", write_buf.size());
			log("99.9%%ile: %10llu ns", ms.percentile(0.999).count());
			log("99.0%%ile: %10llu ns", ms.percentile(0.99).count());
			log("90.0%%ile: %10llu ns", ms.percentile(0.9).count());
			log("50.0%%ile: %10llu ns", ms.percentile(0.5).count());
			log("10.0%%ile: %10llu ns", ms.percentile(0.1).count());
			log(" 1.0%%ile: %10llu ns", ms.percentile(0.01).count());
			log(" 0.1%%ile: %10llu ns", ms.percentile(0.001).count());
		}
	}

	void handle_close()
	{
		this->watcher.shutdown = true;
	}

	void close()
	{
		this->watcher.shutdown = true;
		if (this->watcher.task.joinable()) {
			this->watcher.task.join();
		}
	}

	public:
	client(std::string path)
	{
		datapath::error ec = datapath::connect(dp, path);
		if (ec != datapath::error::Success) {
			throw std::exception();
		}

		write_buf.resize(sizeof(uint64_t));

		this->watcher.shutdown = false;
		this->watcher.task     = std::thread(std::bind(&client::_watcher, this));

		dp->on_close.add(std::bind(&client::handle_close, this));
		dp->on_message.add(std::bind(&client::handle_message, this, std::placeholders::_1));
	}

	~client()
	{
		close();
	}
};

int main(int argc, const char* argv[])
{
	std::shared_ptr<server> myServer = std::make_shared<server>("single-process-ipc");
	std::shared_ptr<client> myClient = std::make_shared<client>("single-process-ipc");

	std::cin.get();
	myClient.reset();
	myServer.reset();

	return 0;
}
