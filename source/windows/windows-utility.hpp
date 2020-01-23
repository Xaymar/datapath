/*
Low Latency IPC Library for high-speed traffic
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

#pragma once
#include <codecvt>
#include <locale>
#include <string>

extern "C" {
#include <Windows.h>
}

namespace datapath {
	namespace windows {
		namespace utility {
			typedef uint64_t packet_size_t;

			static inline bool make_pipe_path(std::string& string)
			{
				// Convert path to WinAPI compatible string.
				if (string.length() >= (MAX_PATH - 10ull)) {
					// \\.\pipe\ is 9 characters, but we count 10 here.
					return false;
				}
				for (char& v : string) {
					if (v == '\\') { // Backslash is not allowed.
						v = '/';
					}
				}
				string = {"\\\\.\\pipe\\" + string};
				return true;
			}

			static inline std::wstring make_wide_string(std::string string)
			{
				std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
				return converter.from_bytes(string);
			}

			static VOID CALLBACK def_io_completion_routine(_In_ DWORD dwErrorCode, _In_ DWORD dwNumberOfBytesTransfered,
														   _Inout_ LPOVERLAPPED lpOverlapped)
			{
				SetEvent(lpOverlapped->hEvent);
			}

			static void shared_ptr_handle_deleter(HANDLE v)
			{
				CloseHandle(v);
			}

			inline void build_packet(const std::vector<char>& source, std::vector<char>& target)
			{
				target.resize(sizeof(packet_size_t) + source.size());
				memcpy(target.data() + sizeof(packet_size_t), source.data(), source.size());
				reinterpret_cast<packet_size_t&>(*target.data()) = source.size();
			}

		} // namespace utility
	}     // namespace windows
} // namespace datapath
