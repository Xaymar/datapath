/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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

TEST(large_frames_reuse_and_stay_intact)
{
	send_queue           q(1 << 24, uint64_t(1) << 30);
	std::vector<uint8_t> batch;
	for (int round = 0; round < 4; round++) {
		size_t               n = size_t(300000 - round * 50000);
		std::vector<uint8_t> p(n, uint8_t(round + 1));
		buffer_view          v{p.data(), p.size()};
		bool                 w = false;
		CHECK(q.push(&v, 1, w) == error::Success);
		CHECK(q.take(batch)); // recycles the previous (large) batch as the spare
		CHECK_EQ(batch.size(), n + datapath::detail::frame_header_size);
		for (size_t i = datapath::detail::frame_header_size; i < batch.size(); i++)
			if (batch[i] != uint8_t(round + 1)) {
				CHECK(false);
				break;
			}
		CHECK(!q.take(batch) || true);
	}
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

TEST(try_acquire_and_state_accessors)
{
	send_queue q(1000, 1 << 20);
	CHECK_EQ(q.limit(), uint64_t(1000));
	q.set_limit(2000);
	CHECK_EQ(q.limit(), uint64_t(2000));
	CHECK(!q.closed());

	CHECK(q.try_acquire());  // nobody holds the slot
	CHECK(!q.try_acquire()); // now we do
	bool w = true;
	CHECK(push(q, 1, w) == error::Success);
	CHECK(!w); // the slot was already taken
	std::vector<uint8_t> batch;
	CHECK(q.take(batch));
	CHECK(!q.take(batch)); // releases the slot
	CHECK(q.try_acquire());
	CHECK(!q.take(batch));

	q.close();
	CHECK(q.closed());
	CHECK(!q.try_acquire());
}

TEST(recycled_blocks_are_reused)
{
	send_queue q(1 << 20, 1 << 20);
	// Recycling keeps only small, non-empty buffers, and only a few of them.
	for (int i = 0; i < 8; i++) {
		std::vector<uint8_t> b;
		b.reserve(1024);
		q.recycle(std::move(b));
	}
	q.recycle(std::vector<uint8_t>());
	std::vector<uint8_t> huge;
	huge.reserve(send_queue::coalesce_limit * 2);
	q.recycle(std::move(huge));

	bool w = false;
	for (uint8_t i = 0; i < 4; i++) {
		CHECK(push(q, 16, w, i) == error::Success);
		std::vector<uint8_t> batch;
		CHECK(q.take(batch));
		CHECK_EQ(batch.size(), size_t(16 + datapath::detail::frame_header_size));
		CHECK_EQ(batch.back(), i);
		q.recycle(std::move(batch));
		CHECK(!q.take(batch));
	}
	q.close();
	q.recycle(std::vector<uint8_t>(16));
}

TEST(direct_send_takes_slot_only_when_idle)
{
	send_queue q(1 << 20, 1 << 20);
	bool       d = false;
	CHECK(q.try_begin_direct((1 << 20) + 1, d) == error::TooLarge);
	CHECK(!d);
	CHECK(q.try_begin_direct(100000, d) == error::Success);
	CHECK(d);
	// Writer slot is held: a second direct send and a push both see that.
	bool d2 = true, w = true;
	CHECK(q.try_begin_direct(100000, d2) == error::Success);
	CHECK(!d2);
	CHECK(push(q, 10, w, 9) == error::Success);
	CHECK(!w);

	// The OS took 3 bytes of header+payload; the rest goes ahead of the queued frame.
	uint8_t     header[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t     a[4] = {10, 11, 12, 13}, b[3] = {20, 21, 22};
	buffer_view parts[] = {{a, 4}, {b, 3}};
	q.push_front_rest(header, 8, parts, 2, 3);
	std::vector<uint8_t> batch;
	CHECK(q.take(batch));
	std::vector<uint8_t> want = {4, 5, 6, 7, 8, 10, 11, 12, 13, 20, 21, 22};
	CHECK(batch == want);
	CHECK(q.take(batch));
	CHECK_EQ(batch.size(), size_t(18)); // the small frame pushed meanwhile
	CHECK(!q.take(batch));

	// All of it sent: nothing is queued. Rest past a part boundary works too.
	CHECK(q.try_begin_direct(7, d) == error::Success);
	CHECK(d);
	q.push_front_rest(header, 8, parts, 2, 15);
	CHECK(!q.take(batch));
	CHECK(q.try_begin_direct(7, d) == error::Success);
	q.push_front_rest(header, 8, parts, 2, 13);
	CHECK(q.take(batch));
	CHECK(batch == (std::vector<uint8_t>{21, 22}));
	CHECK(!q.take(batch));

	q.close();
	CHECK(q.try_begin_direct(7, d) == error::Closed);
}

TEST_MAIN()
