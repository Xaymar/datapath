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

// Bounded, coalescing, multi-producer send queue with a single writer slot.

#pragma once
#include <datapath/error.hpp>
#include <datapath/socket.hpp>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace datapath::detail {
	class send_queue {
		public:
		// Small frames are packed together into blocks up to this size.
		static constexpr size_t coalesce_limit = 64 * 1024;

		private:
		mutable std::mutex                _lock;
		std::deque<std::vector<uint8_t>>  _blocks;
		std::vector<std::vector<uint8_t>> _free;
		// One recycled large block, reused while large messages keep coming.
		std::vector<uint8_t>              _spare_large;
		uint64_t                          _queued = 0;
		uint64_t                          _limit;
		uint64_t                          _max_queued;
		bool                              _writing = false;
		bool                              _closed  = false;

		std::vector<uint8_t> new_block_locked(size_t reserve);

		public:
		send_queue(uint64_t max_message_size, uint64_t max_queued_bytes);

		// Frame and enqueue one message made of `parts`. Atomic: either the whole
		// message is queued or nothing is. `acquired_writer` is set to true for
		// exactly one caller while nobody holds the writer slot; that caller must
		// drain the queue with take() until it returns false.
		datapath::error push(const buffer_view* parts, size_t count, bool& acquired_writer);

		// Writer only. Moves the next block into `batch` (its old storage is
		// recycled) and returns true, or releases the writer slot and returns false
		// when the queue is empty.
		bool take(std::vector<uint8_t>& batch);

		// Writer only: try to acquire the writer slot without pushing (used to resume).
		bool try_acquire();

		void recycle(std::vector<uint8_t>&& buffer);
		void set_limit(uint64_t max_message_size);
		uint64_t limit() const;
		// Drop everything queued; later pushes return Closed.
		void close();
		bool closed() const;
	};
} // namespace datapath::detail
