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

#include <datapath/datapath.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cstring>
#include <unistd.h>
#include <string>
#include "check.hpp"

using datapath::error;

static std::string socket_path(const char* tag)
{
	return "/tmp/datapath-test-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

TEST(linux_permissions_mode_bits)
{
	struct {
		datapath::permissions level;
		mode_t                mode;
	} cases[] = {{datapath::permissions::User, 0600},
				 {datapath::permissions::Group, 0660},
				 {datapath::permissions::World, 0666}};
	for (auto& c : cases) {
		auto              path = socket_path("perm");
		datapath::options o;
		o.permissions = c.level;
		std::shared_ptr<datapath::server> srv;
		CHECK_EQ(datapath::host(srv, path, o), error::Success);
		struct stat st {};
		CHECK_EQ(::stat(path.c_str(), &st), 0);
		CHECK(S_ISSOCK(st.st_mode));
		CHECK_EQ(st.st_mode & 0777, c.mode);
		srv.reset();
		CHECK(::access(path.c_str(), F_OK) != 0); // removed on close
	}
}

TEST(linux_filesystem_socket_roundtrip_and_stale_file)
{
	auto path = socket_path("fs");
	{
		// Leave a stale socket file behind, as a crashed server would.
		int         fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		sockaddr_un a{};
		a.sun_family = AF_UNIX;
		std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
		CHECK_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
		::close(fd);
		CHECK_EQ(::access(path.c_str(), F_OK), 0);
	}
	std::shared_ptr<datapath::server> srv;
	CHECK_EQ(datapath::host(srv, path), error::Success);
	std::shared_ptr<datapath::server> second;
	CHECK_EQ(datapath::host(second, path), error::AddressInUse); // live server is not stolen
	std::shared_ptr<datapath::socket> keep;
	srv->on_accept([&](std::shared_ptr<datapath::socket> s) {
		keep = s;
		return true;
	});
	std::shared_ptr<datapath::socket> c;
	CHECK_EQ(datapath::connect(c, path), error::Success);
	CHECK(c->good());
}

TEST(invalid_paths)
{
	std::shared_ptr<datapath::server> srv;
	CHECK_EQ(datapath::host(srv, ""), error::InvalidPath);
	CHECK_EQ(datapath::host(srv, "/" + std::string(200, 'x')), error::InvalidPath);
	CHECK_EQ(datapath::host(srv, std::string(200, 'x')), error::InvalidPath);
}

TEST_MAIN()
