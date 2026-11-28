/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Bounded, coalescing, multi-producer send queue with a single writer slot.

#pragma once
#include <datapath/error.hpp>
#include <datapath/socket.hpp>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace datapath { namespace detail {
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
		// A push returned WouldBlock since the queue last ran empty.
		bool                              _blocked = false;
		// The queue ran empty after _blocked was set; take_drained() reports it.
		bool                              _drained = false;

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

		// Direct send: if nothing is queued and nobody writes, take the writer slot
		// so the caller can write a message of `total` payload bytes straight from
		// its own buffers (`acquired` = true). Otherwise `acquired` stays false and
		// the caller uses push(). Returns the same errors push() would.
		datapath::error try_begin_direct(uint64_t total, bool& acquired);

		// Writer only, after a direct send the OS only partly took: queue the bytes
		// of `header` followed by `parts` that come after the first `sent`, ahead of
		// anything other threads queued meanwhile. Does nothing once closed.
		void push_front_rest(const uint8_t* header, size_t header_size, const buffer_view* parts, size_t count,
							 size_t sent);

		// True once after a push returned WouldBlock and the writer has since
		// emptied the queue (take() returned false): time to tell the user.
		bool take_drained();

		void recycle(std::vector<uint8_t>&& buffer);
		void set_limit(uint64_t max_message_size);
		uint64_t limit() const;
		// Drop everything queued; later pushes return Closed.
		void close();
		bool closed() const;
	};
}} // namespace datapath::detail
