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

#include "task.hpp"

datapath::windows::task::task()
{
	_overlapped.set_callback(std::bind(&datapath::windows::task::handle_overlapped, this, std::placeholders::_1,
									   std::placeholders::_2, std::placeholders::_3));
}

datapath::windows::task::~task()
{
	cancel();
}

::datapath::windows::overlapped& datapath::windows::task::overlapped()
{
	return _overlapped;
}

std::vector<char>& datapath::windows::task::get_data()
{
	return _data;
}

void datapath::windows::task::handle_overlapped(::datapath::windows::overlapped& ov, size_t bytes, void* ptr)
{
	DWORD _bytes = 0;
	if (GetOverlappedResult(_overlapped.get_handle(), _overlapped.get_overlapped(), &_bytes, false)) {
	}
}

datapath::error datapath::windows::task::cancel()
{
	_overlapped.cancel();
	return datapath::error::Success;
}

bool datapath::windows::task::is_completed()
{
	return _overlapped.is_completed();
}

size_t datapath::windows::task::length()
{
	return _data.size();
}

const std::vector<char>& datapath::windows::task::data()
{
	return _data;
}
