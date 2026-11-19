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

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "check.hpp"
#include "common/framing.hpp"
#include "common/send_queue.hpp"

using datapath::buffer_view;
using datapath::error;
using datapath::detail::send_queue;

static error push(send_queue& q, size_t n, bool& acquired, uint8_t fill = 0)
{
	std::vector<uint8_t> payload(n, fill);
	buffer_view          v{payload.data(), payload.size()};
	return q.push(&v, 1, acquired);
}

TEST(push_rejects_too_large)
{
	send_queue q(100, 1 << 20);
	bool       w = false;
	CHECK(push(q, 101, w) == error::TooLarge);
	CHECK(!w);
	CHECK(push(q, 100, w) == error::Success);
	CHECK(w);

	// The limit applies to the sum of all parts.
	uint8_t     a[60] = {}, b[41] = {};
	buffer_view parts[] = {{a, sizeof(a)}, {b, sizeof(b)}};
	CHECK(q.push(parts, 2, w) == error::TooLarge);

	q.set_limit(200);
	CHECK(q.push(parts, 2, w) == error::Success);
}

TEST(would_block_when_full)
{
	send_queue q(64 << 20, 1 << 20);
	bool       w        = false;
	int        accepted = 0;
	while (push(q, 64 * 1024, w) == error::Success)
		accepted++;
	CHECK_EQ(accepted, 15); // 15 * (64 KiB + 8) <= 1 MiB < 16 * (64 KiB + 8)
	CHECK(push(q, 64 * 1024, w) == error::WouldBlock);
	std::vector<uint8_t> batch;
	CHECK(q.take(batch));
	CHECK(push(q, 64 * 1024, w) == error::Success);
}

TEST(empty_queue_accepts_frame_above_queue_limit)
{
	send_queue q(1 << 20, 1024);
	bool       w = false;
	CHECK(push(q, 4096, w) == error::Success);
	CHECK(push(q, 1, w) == error::WouldBlock);
}

TEST(coalesces_small_frames)
{
	send_queue q(1 << 20, 1 << 20);
	bool       w = false;
	for (int i = 0; i < 100; i++)
		CHECK(push(q, 10, w, uint8_t(i)) == error::Success);
	std::vector<uint8_t> batch;
	CHECK(q.take(batch));
	CHECK_EQ(batch.size(), size_t(100 * (10 + datapath::detail::frame_header_size)));
	CHECK(!q.take(batch));
}

TEST(single_writer_slot)
{
	send_queue q(1 << 20, 1 << 20);
	bool       w1 = false, w2 = false, w3 = false;
	CHECK(push(q, 1, w1) == error::Success);
	CHECK(push(q, 1, w2) == error::Success);
	CHECK(w1);
	CHECK(!w2);
	std::vector<uint8_t> batch;
	CHECK(q.take(batch));
	CHECK(!q.take(batch)); // releases the slot
	CHECK(push(q, 1, w3) == error::Success);
	CHECK(w3);
}

TEST(closed_queue_rejects)
{
	send_queue q(1 << 20, 1 << 20);
	bool       w = false;
	CHECK(push(q, 1, w) == error::Success);
	q.close();
	CHECK(push(q, 1, w) == error::Closed);
	std::vector<uint8_t> batch;
	CHECK(!q.take(batch));
}

TEST(concurrent_push_no_interleave)
{
	constexpr int threads = 8, per_thread = 10000;
	send_queue    q(1 << 20, uint64_t(1) << 40);

	std::mutex           out_lock;
	std::vector<uint8_t> stream;
	std::atomic<int>     writers_active{0};
	std::atomic<bool>    two_writers{false};

	auto drain = [&] {
		if (writers_active.fetch_add(1) != 0)
			two_writers = true;
		std::vector<uint8_t> batch;
		// Decrement before the take() that releases the slot, so a writer that
		// acquires the slot right after the release never sees a stale count.
		for (;;) {
			{
				std::unique_lock<std::mutex> ul(out_lock);
				if (!stream.empty() || !batch.empty())
					stream.insert(stream.end(), batch.begin(), batch.end());
			}
			batch.clear();
			writers_active.fetch_sub(1);
			if (!q.take(batch))
				return;
			if (writers_active.fetch_add(1) != 0)
				two_writers = true;
		}
	};

	std::vector<std::thread> ts;
	for (int t = 0; t < threads; t++) {
		ts.emplace_back([&, t] {
			for (int i = 0; i < per_thread; i++) {
				size_t               len = 5 + size_t((i * 31 + t) % 300);
				std::vector<uint8_t> p(len, uint8_t(i ^ t));
				p[0] = uint8_t(t);
				std::memcpy(&p[1], &i, 4);
				// Occasionally exercise the large (copied outside the lock) path.
				if (i % 997 == 0)
					p.resize(200000, uint8_t(i ^ t));
				buffer_view v{p.data(), p.size()};
				bool        w = false;
				CHECK(q.push(&v, 1, w) == error::Success);
				if (w)
					drain();
			}
		});
	}
	for (auto& t : ts)
		t.join();
	CHECK(!two_writers);

	std::vector<int>                 next(threads, 0);
	size_t                           count = 0;
	datapath::detail::frame_parser   parser(1 << 20, 1 << 16);
	size_t                           pos   = 0;
	bool                             ok    = true;
	while (pos < stream.size()) {
		auto   area = parser.prepare();
		size_t n    = std::min(area.second, stream.size() - pos);
		std::memcpy(area.first, stream.data() + pos, n);
		pos += n;
		auto ec = parser.commit(n, [&](const uint8_t* d, size_t s) {
			int t = d[0], i;
			std::memcpy(&i, d + 1, 4);
			if (t >= threads || i != next[t]) {
				ok = false;
				return;
			}
			size_t want = (i % 997 == 0) ? 200000 : 5 + size_t((i * 31 + t) % 300);
			if (s != want)
				ok = false;
			for (size_t k = 5; k < s; k++)
				if (d[k] != uint8_t(i ^ t)) {
					ok = false;
					break;
				}
			next[t]++;
			count++;
		});
		CHECK(ec == error::Success);
	}
	CHECK(ok);
	CHECK_EQ(count, size_t(threads * per_thread));
}

TEST_MAIN()
