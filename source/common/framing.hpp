/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Wire framing: an 8-byte little-endian length followed by the payload.

#pragma once
#include <datapath/error.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
#include "endian.hpp"

namespace datapath::detail {
	constexpr size_t frame_header_size = 8;

	inline void encode_frame_header(uint8_t* out, uint64_t length) noexcept
	{
		store_le64(out, length);
	}

	// Incremental, zero-copy frame parser for a byte stream.
	//
	//   auto [ptr, len] = parser.prepare();   // read up to len bytes into ptr
	//   parser.commit(bytes_read, deliver);   // deliver(data, size) per complete frame
	//
	// Small frames are delivered straight out of the receive buffer. A frame larger
	// than the buffer gets an exact-size buffer of its own, which subsequent reads
	// fill directly and which is released after delivery.
	class frame_parser {
		std::vector<uint8_t> _buf;
		size_t               _rpos = 0;
		size_t               _wpos = 0;

		// Dedicated buffer for frames larger than _buf. Allocated uninitialised and
		// kept across consecutive large frames (bulk streams); released as soon as a
		// small frame arrives, so idle or chatty connections hold no extra memory.
		std::unique_ptr<uint8_t[]> _big;
		size_t                     _big_cap    = 0;
		size_t                     _big_size   = 0;
		size_t                     _big_filled = 0;
		bool                       _in_big     = false;

		uint64_t _limit;

		// Adaptive read size: small traffic keeps reads small (large read requests
		// are expensive for Windows pipes even when little data arrives); reads that
		// fill the request completely double it, up to the buffer size.
		static constexpr size_t min_read = 64 * 1024;
		size_t                  _read_hint;
		size_t                  _last_request = 0;
		// Bytes still missing from a partly received frame that fits _buf (0 when
		// none or unknown). The next read asks for at least this much, and a read
		// that only finishes such a frame's tail does not shrink the hint: else a
		// frame just over the hint (64 KiB payload + header) is always split into
		// a full read and a tiny one, and the tiny one halves the hint again.
		size_t                  _need = 0;

		void compact() noexcept
		{
			if (_rpos == 0)
				return;
			size_t n = _wpos - _rpos;
			if (n)
				std::memmove(_buf.data(), _buf.data() + _rpos, n);
			_rpos = 0;
			_wpos = n;
		}

		template<typename F>
		static bool call(F& deliver, const uint8_t* data, size_t size)
		{
			if constexpr (std::is_same_v<decltype(deliver(data, size)), bool>) {
				return deliver(data, size);
			} else {
				deliver(data, size);
				return true;
			}
		}

		public:
		frame_parser(uint64_t max_message_size, size_t buffer_size)
			: _buf(buffer_size < 64 ? 64 : buffer_size), _limit(max_message_size),
			  _read_hint(buffer_size < min_read ? (buffer_size < 64 ? 64 : buffer_size) : min_read)
		{}

		void set_limit(uint64_t limit) noexcept
		{
			_limit = limit;
		}

		// Where the next read should go, and how many bytes it may read.
		std::pair<uint8_t*, size_t> prepare() noexcept
		{
			if (_in_big)
				return {_big.get() + _big_filled, _big_size - _big_filled};
			if (_rpos == _wpos) {
				_rpos = _wpos = 0;
			} else if (_wpos == _buf.size() || (_buf.size() - _wpos) < frame_header_size) {
				compact();
			}
			size_t room   = _buf.size() - _wpos;
			size_t want   = _need > _read_hint ? _need : _read_hint;
			_last_request = room < want ? room : want;
			return {_buf.data() + _wpos, _last_request};
		}

		// Account for `n` bytes read into the area returned by prepare() and deliver
		// every complete frame. `deliver` may return bool; false stops parsing and
		// yields error::Closed (used when the connection was closed from a handler).
		template<typename F>
		datapath::error commit(size_t n, F&& deliver)
		{
			if (_in_big) {
				_big_filled += n;
				if (_big_filled < _big_size)
					return datapath::error::Success;
				_in_big     = false;
				_big_filled = 0;
				if (!call(deliver, _big.get(), _big_size))
					return datapath::error::Closed;
				return datapath::error::Success;
			}

			_wpos += n;
			bool tail = _need != 0 && n <= _need;
			_need     = 0;
			if (n == _last_request && _read_hint < _buf.size()) {
				_read_hint = (_read_hint * 2 < _buf.size()) ? _read_hint * 2 : _buf.size();
			} else if (!tail && n < _read_hint / 8 && _read_hint > min_read) {
				_read_hint /= 2;
			}
			for (;;) {
				size_t avail = _wpos - _rpos;
				if (avail < frame_header_size)
					break;
				uint64_t len = load_le64(_buf.data() + _rpos);
				if (len > _limit)
					return datapath::error::TooLarge;
				if (avail - frame_header_size >= len) {
					if (_big) {
						// Back to small frames: give the large buffer back.
						_big.reset();
						_big_cap = 0;
					}
					const uint8_t* data = _buf.data() + _rpos + frame_header_size;
					_rpos += frame_header_size + size_t(len);
					if (!call(deliver, data, size_t(len)))
						return datapath::error::Closed;
					continue;
				}
				if (frame_header_size + len <= _buf.size()) {
					// Fits the buffer; make room for the rest and wait for more data.
					if (_rpos + frame_header_size + len > _buf.size())
						compact();
					_need = frame_header_size + size_t(len) - avail;
					break;
				}
				// Larger than the receive buffer: move to a dedicated buffer.
				if (_big_cap < len) {
					_big.reset();
					_big_cap = 0;
					_big.reset(new (std::nothrow) uint8_t[size_t(len)]);
					if (!_big)
						return datapath::error::Failure;
					_big_cap = size_t(len);
				}
				_big_size   = size_t(len);
				size_t have = avail - frame_header_size;
				std::memcpy(_big.get(), _buf.data() + _rpos + frame_header_size, have);
				_big_filled = have;
				_rpos = _wpos = 0;
				_in_big       = true;
				break;
			}
			return datapath::error::Success;
		}
	};
} // namespace datapath::detail
