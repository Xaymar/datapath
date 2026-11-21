/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "check.hpp"
#include "common/framing.hpp"
#include "common/handshake.hpp"
#include "common/spin.hpp"

using datapath::error;
using bytes = std::vector<uint8_t>;

static bytes frame(const bytes& payload)
{
	bytes out(datapath::detail::frame_header_size + payload.size());
	datapath::detail::encode_frame_header(out.data(), payload.size());
	if (!payload.empty())
		std::memcpy(out.data() + datapath::detail::frame_header_size, payload.data(), payload.size());
	return out;
}

static bytes pattern(size_t n, uint8_t seed)
{
	bytes b(n);
	for (size_t i = 0; i < n; i++)
		b[i] = uint8_t(seed + i * 7);
	return b;
}

// Feed `stream` to a parser in the given chunk sizes; return the delivered messages.
static std::vector<bytes> feed(const bytes& stream, const std::vector<size_t>& chunks, size_t buffer_size,
							   uint64_t limit = 1ull << 30, error* status = nullptr)
{
	datapath::detail::frame_parser p(limit, buffer_size);
	std::vector<bytes>             out;
	size_t                         pos = 0, ci = 0;
	while (pos < stream.size()) {
		size_t want = chunks.empty() ? stream.size() : chunks[ci++ % chunks.size()];
		auto   area = p.prepare();
		size_t n    = std::min({want, area.second, stream.size() - pos});
		CHECK(area.second > 0);
		std::memcpy(area.first, stream.data() + pos, n);
		pos += n;
		error ec = p.commit(n, [&](const uint8_t* d, size_t s) { out.emplace_back(d, d + s); });
		if (ec != error::Success) {
			if (status)
				*status = ec;
			return out;
		}
	}
	if (status)
		*status = error::Success;
	return out;
}

TEST(parser_every_split_offset)
{
	std::vector<bytes> msgs = {bytes{}, pattern(5, 1), pattern(70000, 2)};
	bytes              stream;
	for (auto& m : msgs) {
		auto f = frame(m);
		stream.insert(stream.end(), f.begin(), f.end());
	}
	// Byte by byte.
	CHECK(feed(stream, {1}, 1024) == msgs);
	// Every two-way split.
	for (size_t split = 1; split < stream.size(); split += (split < 64 ? 1 : 997)) {
		CHECK(feed(stream, {split, stream.size()}, 1024) == msgs);
	}
	// Buffer smaller than a header-sized read still works.
	CHECK(feed(stream, {3}, 16) == msgs);
}

TEST(parser_many_small_per_read)
{
	std::vector<bytes> msgs;
	bytes              stream;
	for (int i = 0; i < 1000; i++) {
		msgs.push_back(pattern(size_t(i % 17), uint8_t(i)));
		auto f = frame(msgs.back());
		stream.insert(stream.end(), f.begin(), f.end());
	}
	CHECK(feed(stream, {}, 64 * 1024) == msgs);
}

TEST(parser_large_frame_beyond_buffer)
{
	std::vector<bytes> msgs = {pattern(3 << 20, 9), pattern(10, 3)};
	bytes              stream;
	for (auto& m : msgs) {
		auto f = frame(m);
		stream.insert(stream.end(), f.begin(), f.end());
	}
	CHECK(feed(stream, {65536}, 65536) == msgs);
}

TEST(parser_alternating_large_and_small_frames)
{
	// Large buffers are reused across consecutive large frames, released on small
	// ones, and must never leak bytes between messages.
	std::vector<bytes> msgs = {pattern(200000, 1), pattern(150000, 2), pattern(300000, 3), pattern(7, 4),
							   pattern(250000, 5), bytes{},          pattern(100000, 6)};
	bytes              stream;
	for (auto& m : msgs) {
		auto f = frame(m);
		stream.insert(stream.end(), f.begin(), f.end());
	}
	CHECK(feed(stream, {65536}, 65536) == msgs);
	CHECK(feed(stream, {999}, 4096) == msgs);
}

TEST(parser_read_size_adapts)
{
	datapath::detail::frame_parser p(1ull << 30, 1 << 20);
	CHECK_EQ(p.prepare().second, size_t(64 * 1024)); // starts small
	// Reads that fill the request grow it (bulk traffic)...
	bytes  big = frame(pattern(900000, 1));
	size_t pos = 0, last = 0;
	while (pos < big.size()) {
		auto   a = p.prepare();
		size_t n = std::min(a.second, big.size() - pos);
		std::memcpy(a.first, big.data() + pos, n);
		pos += n;
		last = std::max(last, a.second);
		CHECK(p.commit(n, [](const uint8_t*, size_t) {}) == error::Success);
	}
	CHECK(last > size_t(64 * 1024));
	// ...and small reads shrink it back.
	for (int i = 0; i < 20; i++) {
		auto f = frame(pattern(8, 2));
		auto a = p.prepare();
		std::memcpy(a.first, f.data(), f.size());
		CHECK(p.commit(f.size(), [](const uint8_t*, size_t) {}) == error::Success);
	}
	CHECK_EQ(p.prepare().second, size_t(64 * 1024));
}

TEST(parser_64k_frames_take_one_read)
{
	// A 64 KiB payload plus header is just over the starting read size. After the
	// first frame, the read size must stay large enough to take each frame in one
	// read instead of a full read plus an 8-byte one.
	datapath::detail::frame_parser p(1ull << 30, 256 * 1024);
	bytes  f     = frame(pattern(65536, 3));
	size_t reads = 0, msgs = 0;
	for (int i = 0; i < 50; i++) {
		size_t pos = 0;
		while (pos < f.size()) {
			auto   a = p.prepare();
			size_t n = std::min(a.second, f.size() - pos); // the peer sent exactly one frame
			std::memcpy(a.first, f.data() + pos, n);
			pos += n;
			if (i >= 2)
				reads++;
			CHECK(p.commit(n, [&](const uint8_t*, size_t s) { msgs += s == 65536; }) == error::Success);
		}
	}
	CHECK_EQ(msgs, size_t(50));
	CHECK_EQ(reads, size_t(48));
}

TEST(parser_rejects_oversize_header)
{
	error st = error::Unknown;
	auto  ok = frame(pattern(1024, 0));
	CHECK(feed(ok, {}, 4096, 1024, &st).size() == 1);
	CHECK(st == error::Success);

	bytes header(datapath::detail::frame_header_size);
	datapath::detail::encode_frame_header(header.data(), 1025);
	auto got = feed(header, {}, 4096, 1024, &st);
	CHECK(got.empty());
	CHECK(st == error::TooLarge);

	// A huge claimed size is rejected without trying to allocate it.
	datapath::detail::encode_frame_header(header.data(), ~uint64_t(0));
	got = feed(header, {}, 4096, 1024, &st);
	CHECK(st == error::TooLarge);
}

TEST(hello_roundtrip)
{
	uint8_t                  buf[datapath::detail::hello_size];
	datapath::detail::hello  in{datapath::detail::protocol_version, 123456789012ull};
	datapath::detail::hello  out{};
	datapath::detail::encode_hello(buf, in);
	CHECK(std::memcmp(buf, "DPTH", 4) == 0);
	CHECK(datapath::detail::decode_hello(buf, out) == error::Success);
	CHECK_EQ(out.version, in.version);
	CHECK_EQ(out.max_message_size, in.max_message_size);
}

TEST(hello_bad_magic)
{
	uint8_t                 buf[datapath::detail::hello_size];
	datapath::detail::hello h{datapath::detail::protocol_version, 1};
	datapath::detail::encode_hello(buf, h);
	buf[0] = 'X';
	CHECK(datapath::detail::decode_hello(buf, h) == error::VersionMismatch);
}

TEST(hello_bad_version)
{
	uint8_t                 buf[datapath::detail::hello_size];
	datapath::detail::hello h{datapath::detail::protocol_version + 1, 1};
	datapath::detail::encode_hello(buf, h);
	CHECK(datapath::detail::decode_hello(buf, h) == error::VersionMismatch);
}

TEST(spin_zero_budget_blocks_immediately)
{
	int polls = 0, blocks = 0;
	datapath::detail::spin_then_block(
		std::chrono::microseconds(0),
		[&] {
			polls++;
			return false;
		},
		[&] { blocks++; });
	CHECK_EQ(blocks, 1);
	CHECK(polls <= 1);
}

TEST(spin_work_defers_block)
{
	using clock = std::chrono::steady_clock;
	int  work   = 5, blocks = 0;
	auto start  = clock::now();
	datapath::detail::spin_then_block(
		std::chrono::microseconds(2000),
		[&] {
			if (work > 0) {
				work--;
				return true;
			}
			return false;
		},
		[&] { blocks++; });
	auto took = clock::now() - start;
	CHECK_EQ(work, 0);
	CHECK_EQ(blocks, 1);
	// Spun for about the budget after the last work item, not forever.
	CHECK(took >= std::chrono::microseconds(1500));
	CHECK(took < std::chrono::seconds(1));
}

// A deliver callback returning false stops parsing with Closed, for frames that
// fit the buffer and for frames that need the dedicated large buffer.
TEST(parser_deliver_false_stops)
{
	for (size_t size : {size_t(10), size_t(200000)}) {
		bytes stream;
		for (int i = 0; i < 3; i++) {
			auto f = frame(pattern(size, uint8_t(i)));
			stream.insert(stream.end(), f.begin(), f.end());
		}
		datapath::detail::frame_parser p(1ull << 30, 4096);
		int                            calls = 0;
		error                          ec    = error::Success;
		size_t                         pos   = 0;
		while (pos < stream.size() && ec == error::Success) {
			auto   a = p.prepare();
			size_t n = std::min(a.second, stream.size() - pos);
			std::memcpy(a.first, stream.data() + pos, n);
			pos += n;
			ec = p.commit(n, [&](const uint8_t*, size_t s) -> bool {
				CHECK_EQ(s, size);
				return ++calls < 2;
			});
		}
		CHECK_EQ(ec, error::Closed);
		CHECK_EQ(calls, 2);
	}
}

TEST(parser_set_limit_applies_to_next_frame)
{
	datapath::detail::frame_parser p(16, 4096);
	auto                           f = frame(pattern(32, 1));
	p.set_limit(64);
	auto a = p.prepare();
	std::memcpy(a.first, f.data(), f.size());
	int got = 0;
	CHECK_EQ(p.commit(f.size(), [&](const uint8_t*, size_t) { got++; }), error::Success);
	CHECK_EQ(got, 1);
	p.set_limit(8);
	a = p.prepare();
	std::memcpy(a.first, f.data(), f.size());
	CHECK_EQ(p.commit(f.size(), [&](const uint8_t*, size_t) { got++; }), error::TooLarge);
	CHECK_EQ(got, 1);
}

// Tiny buffers are clamped to a usable minimum.
TEST(parser_tiny_buffer_is_clamped)
{
	datapath::detail::frame_parser p(1ull << 30, 1);
	CHECK(p.prepare().second >= size_t(64));
	std::vector<bytes> msgs = {pattern(40, 1), pattern(100, 2), pattern(3, 3)};
	bytes              stream;
	for (auto& m : msgs) {
		auto f = frame(m);
		stream.insert(stream.end(), f.begin(), f.end());
	}
	CHECK(feed(stream, {7}, 1) == msgs);
}

TEST_MAIN()
