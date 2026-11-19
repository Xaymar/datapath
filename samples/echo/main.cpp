/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

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

// Minimal DataPath example: host a server, connect, exchange one message.

#include <datapath/datapath.hpp>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

int main()
{
	std::mutex              lock;
	std::condition_variable cv;
	std::string             reply;

	// Server: echo every message back in upper case.
	std::shared_ptr<datapath::server> server;
	if (datapath::host(server, "datapath-echo-example") != datapath::error::Success) {
		std::fprintf(stderr, "could not host\n");
		return 1;
	}
	std::shared_ptr<datapath::socket> peer; // keep accepted clients alive
	server->on_accept([&](std::shared_ptr<datapath::socket> client) {
		datapath::socket* raw = client.get(); // no shared_ptr capture: avoids a cycle
		client->on_message([raw](const uint8_t* data, size_t size) {
			std::string s(reinterpret_cast<const char*>(data), size);
			for (char& c : s)
				c = char(std::toupper(static_cast<unsigned char>(c)));
			raw->send(s.data(), s.size());
		});
		peer = client;
		return true;
	});

	// Client.
	std::shared_ptr<datapath::socket> client;
	if (auto ec = datapath::connect(client, "datapath-echo-example"); ec != datapath::error::Success) {
		std::fprintf(stderr, "connect failed: %s\n", datapath::to_string(ec));
		return 1;
	}
	client->on_message([&](const uint8_t* data, size_t size) {
		std::lock_guard<std::mutex> l(lock);
		reply.assign(reinterpret_cast<const char*>(data), size);
		cv.notify_all();
	});

	const char msg[] = "hello, datapath";
	client->send(msg, sizeof(msg) - 1);

	std::unique_lock<std::mutex> l(lock);
	cv.wait(l, [&] { return !reply.empty(); });
	std::printf("%s\n", reply.c_str());
	return 0;
}
