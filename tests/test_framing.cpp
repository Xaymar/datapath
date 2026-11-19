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

TEST_MAIN()
