/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "send_queue.hpp"
#include <cstring>
#include "framing.hpp"

datapath::detail::send_queue::send_queue(uint64_t max_message_size, uint64_t max_queued_bytes)
	: _limit(max_message_size), _max_queued(max_queued_bytes)
{}

std::vector<uint8_t> datapath::detail::send_queue::new_block_locked(size_t reserve)
{
	std::vector<uint8_t> block;
	if (reserve <= coalesce_limit && !_free.empty()) {
		block = std::move(_free.back());
		_free.pop_back();
		block.clear();
	}
	block.reserve(reserve);
	return block;
}

datapath::error datapath::detail::send_queue::push(const buffer_view* parts, size_t count, bool& acquired_writer)
{
	acquired_writer = false;
	uint64_t total  = 0;
	for (size_t i = 0; i < count; i++)
		total += parts[i].size;
	const uint64_t frame = total + frame_header_size;

	// Append without zero-filling first: insert() copies straight into place.
	auto append = [&](std::vector<uint8_t>& block) {
		uint8_t header[frame_header_size];
		encode_frame_header(header, total);
		block.insert(block.end(), header, header + frame_header_size);
		for (size_t i = 0; i < count; i++) {
			auto* p = static_cast<const uint8_t*>(parts[i].data);
			if (parts[i].size)
				block.insert(block.end(), p, p + parts[i].size);
		}
	};

	std::unique_lock<std::mutex> ul(_lock);
	if (_closed)
		return datapath::error::Closed;
	if (total > _limit)
		return datapath::error::TooLarge;
	if (_queued > 0 && _queued + frame > _max_queued)
		return datapath::error::WouldBlock;

	if (frame <= coalesce_limit) {
		if (_spare_large.capacity())
			_spare_large = std::vector<uint8_t>(); // back to small traffic: release it
		// Small: pack into the tail block when it has room, all under the lock.
		if (_blocks.empty() || _blocks.back().size() + frame > coalesce_limit
			|| _blocks.back().capacity() > coalesce_limit) {
			_blocks.push_back(new_block_locked(coalesce_limit));
		}
		append(_blocks.back());
		_queued += frame;
	} else {
		// Large: reserve queue space, copy outside the lock, then publish.
		_queued += frame;
		std::vector<uint8_t> block;
		if (_spare_large.capacity() >= frame) {
			block = std::move(_spare_large);
			block.clear();
		}
		ul.unlock();
		try {
			block.reserve(size_t(frame));
			append(block);
		} catch (...) {
			ul.lock();
			_queued -= frame;
			return datapath::error::Failure;
		}
		ul.lock();
		if (_closed)
			return datapath::error::Closed;
		_blocks.push_back(std::move(block));
	}

	if (!_writing) {
		_writing        = true;
		acquired_writer = true;
	}
	return datapath::error::Success;
}

bool datapath::detail::send_queue::take(std::vector<uint8_t>& batch)
{
	std::unique_lock<std::mutex> ul(_lock);
	if (batch.capacity() > 0 && batch.capacity() <= coalesce_limit && _free.size() < 4) {
		batch.clear();
		_free.push_back(std::move(batch));
	} else if (batch.capacity() > coalesce_limit && !_closed) {
		_spare_large = std::move(batch);
	}
	batch = std::vector<uint8_t>();
	if (_blocks.empty() || _closed) {
		_writing = false;
		return false;
	}
	batch = std::move(_blocks.front());
	_blocks.pop_front();
	_queued -= batch.size();
	return true;
}

bool datapath::detail::send_queue::try_acquire()
{
	std::unique_lock<std::mutex> ul(_lock);
	if (_writing || _closed)
		return false;
	_writing = true;
	return true;
}

datapath::error datapath::detail::send_queue::try_begin_direct(uint64_t total, bool& acquired)
{
	acquired = false;
	std::unique_lock<std::mutex> ul(_lock);
	if (_closed)
		return datapath::error::Closed;
	if (total > _limit)
		return datapath::error::TooLarge;
	if (_writing || !_blocks.empty())
		return datapath::error::Success;
	_writing = true;
	acquired = true;
	return datapath::error::Success;
}

void datapath::detail::send_queue::push_front_rest(const uint8_t* header, size_t header_size,
												  const buffer_view* parts, size_t count, size_t sent)
{
	size_t total = header_size;
	for (size_t i = 0; i < count; i++)
		total += parts[i].size;
	if (sent >= total)
		return;
	std::vector<uint8_t> block;
	{
		std::unique_lock<std::mutex> ul(_lock);
		if (_closed)
			return;
		if (_spare_large.capacity() >= total - sent)
			block = std::move(_spare_large);
	}
	block.clear();
	block.reserve(total - sent);
	auto append = [&](const uint8_t* p, size_t n) {
		if (sent >= n) {
			sent -= n;
			return;
		}
		block.insert(block.end(), p + sent, p + n);
		sent = 0;
	};
	append(header, header_size);
	for (size_t i = 0; i < count; i++)
		append(static_cast<const uint8_t*>(parts[i].data), parts[i].size);
	std::unique_lock<std::mutex> ul(_lock);
	if (_closed)
		return;
	_queued += block.size();
	_blocks.push_front(std::move(block));
}

void datapath::detail::send_queue::recycle(std::vector<uint8_t>&& buffer)
{
	std::unique_lock<std::mutex> ul(_lock);
	if (buffer.capacity() > 0 && buffer.capacity() <= coalesce_limit && _free.size() < 4) {
		buffer.clear();
		_free.push_back(std::move(buffer));
	}
}

void datapath::detail::send_queue::set_limit(uint64_t max_message_size)
{
	std::unique_lock<std::mutex> ul(_lock);
	_limit = max_message_size;
}

uint64_t datapath::detail::send_queue::limit() const
{
	std::unique_lock<std::mutex> ul(_lock);
	return _limit;
}

void datapath::detail::send_queue::close()
{
	std::unique_lock<std::mutex> ul(_lock);
	_closed = true;
	_blocks.clear();
	_free.clear();
	_spare_large = std::vector<uint8_t>();
	_queued      = 0;
}

bool datapath::detail::send_queue::closed() const
{
	std::unique_lock<std::mutex> ul(_lock);
	return _closed;
}
