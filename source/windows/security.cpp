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

#include "security.hpp"
#include <sddl.h>
#include <vector>

std::wstring datapath::windows::make_sddl(datapath::permissions level, const std::wstring& user_sid)
{
	// Owner and SYSTEM get full access; others get read/write (enough to connect,
	// not enough to create competing pipe instances).
	std::wstring sddl = L"D:P(A;;GA;;;" + user_sid + L")(A;;GA;;;SY)";
	switch (level) {
	case datapath::permissions::User:
		break;
	case datapath::permissions::Group:
		sddl += L"(A;;GRGW;;;BU)";
		break;
	case datapath::permissions::World:
		sddl += L"(A;;GRGW;;;WD)";
		break;
	}
	return sddl;
}

std::wstring datapath::windows::current_user_sid()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return {};
	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	std::vector<uint8_t> buf(size ? size : 1);
	std::wstring         result;
	if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
		auto*  user = reinterpret_cast<TOKEN_USER*>(buf.data());
		LPWSTR str  = nullptr;
		if (ConvertSidToStringSidW(user->User.Sid, &str)) {
			result = str;
			LocalFree(str);
		}
	}
	CloseHandle(token);
	return result;
}

PSECURITY_DESCRIPTOR datapath::windows::make_security_descriptor(datapath::permissions level)
{
	std::wstring sid = current_user_sid();
	if (sid.empty())
		return nullptr;
	std::wstring         sddl = make_sddl(level, sid);
	PSECURITY_DESCRIPTOR sd   = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
		return nullptr;
	return sd;
}
