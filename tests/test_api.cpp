/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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
	CHECK(!co.mmcss);
	CHECK_EQ(co.ideal_core, -1);
	CHECK_EQ(co.priority, 2);
	CHECK(!co.yield);

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
