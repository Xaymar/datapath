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

#define SIZE_ELEMENT uint32_t

void datapath::windows::task::_assign(const std::vector<char>& data, std::shared_ptr<datapath::windows::overlapped> ov){
	this->buffer.resize(data.size() + sizeof(SIZE_ELEMENT));
	std::memcpy(buffer.data() + sizeof(SIZE_ELEMENT), data.data(), data.size());
	reinterpret_cast<SIZE_ELEMENT&>(buffer[0]) = SIZE_ELEMENT(data.size());
	this->overlapped = ov;
}

datapath::windows::task::task()
{
	this->on_wait_error.add([this](datapath::error ec) { this->on_failure(ec); });
	this->on_wait_success.add([this](datapath::error ec) { this->on_success(ec, this->data()); });
}

datapath::windows::task::~task()
{
	cancel();
}

datapath::error datapath::windows::task::cancel()
{
	if (!overlapped) {
		return datapath::error::Unknown;
	}
	if (!overlapped->is_completed()) {
		overlapped->cancel();
		return datapath::error::Success;
	}
	return datapath::error::Failure;
}

bool datapath::windows::task::is_completed()
{
	if (!overlapped) {
		return false;
	}
	return overlapped->is_completed();
}

size_t datapath::windows::task::length()
{
	return buffer.size();
}

const std::vector<char>& datapath::windows::task::data()
{
	return buffer;
}

void* datapath::windows::task::get_waitable()
{
	if (!overlapped) {
		return nullptr;
	}
	return overlapped->get_waitable();
}
