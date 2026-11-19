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

#include "check.hpp"
#include "windows/security.hpp"

using datapath::permissions;
using namespace datapath::windows;

TEST(sddl_levels)
{
	std::wstring sid = L"S-1-5-21-1-2-3-1001";
	auto         u   = make_sddl(permissions::User, sid);
	auto         g   = make_sddl(permissions::Group, sid);
	auto         w   = make_sddl(permissions::World, sid);
	CHECK(u == L"D:P(A;;GA;;;S-1-5-21-1-2-3-1001)(A;;GA;;;SY)");
	CHECK(g == u + L"(A;;GRGW;;;BU)");
	CHECK(w == u + L"(A;;GRGW;;;WD)");
}

TEST(security_descriptor_builds_for_each_level)
{
	CHECK(!current_user_sid().empty());
	for (auto level : {permissions::User, permissions::Group, permissions::World}) {
		PSECURITY_DESCRIPTOR sd = make_security_descriptor(level);
		CHECK(sd != nullptr);
		CHECK(IsValidSecurityDescriptor(sd));
		LocalFree(sd);
	}
}

TEST_MAIN()
