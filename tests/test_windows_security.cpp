/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include <cstdio>
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

#include <datapath/datapath.hpp>
#include <thread>
#include "common/handshake.hpp"

TEST(client_limits_server_to_identification)
{
	// A server must not be able to act with the client's token (named-pipe
	// impersonation). Play the server with raw Win32 calls and inspect the level.
	if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
		std::printf("SKIP: Wine does not implement impersonation levels for pipes\n");
		return;
	}
	std::wstring path = L"\\\\.\\pipe\\dp-test-sqos-" + std::to_wstring(GetCurrentProcessId());
	HANDLE       h    = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0,
										 nullptr);
	CHECK(h != INVALID_HANDLE_VALUE);
	int         level  = -1;
	std::thread server([&] {
		ConnectNamedPipe(h, nullptr);
		uint8_t hello[datapath::detail::hello_size];
		DWORD   got = 0;
		ReadFile(h, hello, sizeof(hello), &got, nullptr);
		if (ImpersonateNamedPipeClient(h)) {
			HANDLE tok = nullptr;
			if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok)) {
				SECURITY_IMPERSONATION_LEVEL l;
				DWORD                        sz = 0;
				if (GetTokenInformation(tok, TokenImpersonationLevel, &l, sizeof(l), &sz))
					level = int(l);
				CloseHandle(tok);
			} else if (GetLastError() == ERROR_CANT_OPEN_ANONYMOUS) {
				level = int(SecurityAnonymous);
			}
			RevertToSelf();
		}
		datapath::detail::encode_hello(hello, {datapath::detail::protocol_version, 1024});
		WriteFile(h, hello, sizeof(hello), &got, nullptr);
	});
	std::shared_ptr<datapath::socket> c;
	std::string                       name = "dp-test-sqos-" + std::to_string(GetCurrentProcessId());
	datapath::options                 o;
	o.shared_memory = false; // the raw server above answers a version 1 hello only
	CHECK_EQ(datapath::connect(c, name, o), datapath::error::Success);
	server.join();
	CloseHandle(h);
	std::printf("impersonation level seen by server: %d\n", level);
	CHECK(level == int(SecurityIdentification) || level == int(SecurityAnonymous));
}

TEST_MAIN()
