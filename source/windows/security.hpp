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

#pragma once
#include <datapath/permissions.hpp>
#include <string>
#include "win.hpp"

namespace datapath::windows {
	// SDDL string for a permission level, for the given user SID string.
	std::wstring make_sddl(datapath::permissions level, const std::wstring& user_sid);
	// The current process user's SID as a string, or empty on failure.
	std::wstring current_user_sid();
	// Security descriptor for named pipes; free with LocalFree. nullptr on failure.
	PSECURITY_DESCRIPTOR make_security_descriptor(datapath::permissions level);
} // namespace datapath::windows
