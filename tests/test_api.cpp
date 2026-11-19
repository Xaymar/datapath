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
#include <set>
#include <string>
#include "check.hpp"

TEST(error_to_string)
{
	using datapath::error;
	const error all[] = {error::Unknown,     error::Success,     error::Failure,         error::CriticalFailure,
						 error::Closed,      error::TimedOut,    error::InvalidPath,     error::NotSupported,
						 error::TooLarge,    error::WouldBlock,  error::VersionMismatch, error::NotFound,
						 error::AddressInUse, error::AccessDenied};
	std::set<std::string> seen;
	for (auto e : all) {
		const char* s = datapath::to_string(e);
		CHECK(s != nullptr);
		CHECK(s[0] != '\0');
		seen.insert(s);
	}
	CHECK_EQ(seen.size(), sizeof(all) / sizeof(all[0]));
}

TEST(options_defaults)
{
	datapath::context_options co;
	CHECK_EQ(co.spin.count(), 50);
	CHECK_EQ(co.io_threads, size_t(1));
	CHECK(!co.alertable);
#ifdef _WIN32
	CHECK(co.yield);
#else
	CHECK(!co.yield);
#endif

	datapath::options o;
	CHECK_EQ(o.max_message_size, uint64_t(64) << 20);
	CHECK_EQ(o.max_queued_bytes, uint64_t(64) << 20);
	CHECK(o.permissions == datapath::permissions::User);
	CHECK_EQ(o.backlog, size_t(8));
	CHECK_EQ(o.max_clients, size_t(0));
	CHECK_EQ(o.connect_timeout.count(), 1000);
	CHECK_EQ(o.receive_buffer, size_t(256) * 1024);
	CHECK_EQ(o.pipe_buffer, size_t(1) << 20);
}

TEST(default_context_is_shared)
{
	auto a = datapath::context::get_default();
	auto b = datapath::context::get_default();
	CHECK(a != nullptr);
	CHECK(a == b);
	auto c = datapath::context::create();
	CHECK(c != nullptr);
	CHECK(c != a);
}

TEST_MAIN()
