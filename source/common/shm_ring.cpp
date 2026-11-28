/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "shm_ring.hpp"
#include <algorithm>
#include <cstring>
#include <new>

namespace {
	using namespace datapath::detail::shm;

	constexpr uint64_t rec_more = uint64_t(1) << 32;
	constexpr uint64_t rec_pad  = uint64_t(1) << 33; // low 32 bits: bytes to skip (0 = to the ring start)
	constexpr uint64_t page     = 4096;

	uint64_t align8(uint64_t v) noexcept
	{
		return (v + 7) & ~uint64_t(7);
	}
} // namespace

// ThreadSanitizer tracks synchronization by address, but the two ends map the
// section at different addresses even inside one process (tests). Tell it
// about each ring's hand-over under a key both ends share.
#if defined(__SANITIZE_THREAD__)
#define DATAPATH_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DATAPATH_TSAN 1
#endif
#endif
#ifdef DATAPATH_TSAN
extern "C" void __tsan_acquire(void* addr);
extern "C" void __tsan_release(void* addr);
#define DATAPATH_TSAN_ACQUIRE(key) (key ? __tsan_acquire(const_cast<void*>(key)) : (void)0)
#define DATAPATH_TSAN_RELEASE(key) (key ? __tsan_release(const_cast<void*>(key)) : (void)0)
#else
#define DATAPATH_TSAN_ACQUIRE(key) ((void)0)
#define DATAPATH_TSAN_RELEASE(key) ((void)0)
#endif

uint64_t datapath::detail::shm::ring_capacity(uint64_t requested) noexcept
{
	uint64_t cap = min_ring;
	while (cap < requested && cap < max_ring)
		cap <<= 1;
	return cap;
}

datapath::detail::shm::section_header* datapath::detail::shm::init_header(void* view, uint64_t cap,
																		  uint32_t flags) noexcept
{
	auto* hdr     = new (view) section_header{};
	hdr->magic    = magic;
	hdr->version  = version;
	hdr->capacity = cap;
	hdr->flags    = flags;
	return hdr;
}

bool datapath::detail::shm::header_matches(const void* view, uint64_t cap, uint32_t flags) noexcept
{
	auto* hdr = static_cast<const section_header*>(view);
	return hdr->magic == magic && hdr->version == version && hdr->capacity == cap && hdr->flags == flags;
}

datapath::detail::shm::ring_pair::ring_pair(const setup& s)
	: _hdr(s.header), _data{s.data[0], s.data[1]}, _cap(s.cap), _mirror(s.mirrored),
	  _other_user(s.other_user), _copy(s.other_user || !s.zero_copy),
	  _tx(s.server ? 0 : 1), _rx(s.server ? 1 : 0), _limit(s.limit), _max_queued(s.max_queued), _signal(s.signal),
	  _signal_ctx(s.signal_ctx), _sync_key{s.sync_key[0], s.sync_key[1]}
{}

// Walks the parts of one message.
struct datapath::detail::shm::ring_pair::cursor {
	const buffer_view* parts;
	size_t             count;
	size_t             i   = 0;
	size_t             off = 0;

	cursor(const buffer_view* p, size_t n) noexcept : parts(p), count(n) {}

	const uint8_t* ptr()
	{
		while (i < count && off == parts[i].size)
			i++, off = 0;
		return i < count ? static_cast<const uint8_t*>(parts[i].data) + off : nullptr;
	}
	void copy(uint8_t* dst, uint64_t n)
	{
		while (n) {
			const uint8_t* p = ptr();
			size_t         k = size_t(std::min<uint64_t>(n, parts[i].size - off));
			std::memcpy(dst, p, k);
			dst += k;
			off += k;
			n -= k;
		}
	}
};

// ---------------------------------------------------------------- writer

namespace {
	// Bytes of padding in front of a record whose header would go at `pos`, so
	// that a payload of 4 KiB or more lands at the source's offset in its page.
	uint64_t gap_for(uint64_t pos, const uint8_t* src, uint64_t len) noexcept
	{
		if (len < page || !src)
			return 0;
		uint64_t want = (reinterpret_cast<uintptr_t>(src) & (page - 1)) & ~uint64_t(7);
		uint64_t cur  = (pos + 8) & (page - 1);
		return (want - cur) & (page - 1);
	}
} // namespace

uint64_t datapath::detail::shm::ring_pair::free_space()
{
	auto&    tx   = _hdr->ring[_tx];
	uint64_t used = _head - tx.tail.load(std::memory_order_acquire);
	DATAPATH_TSAN_ACQUIRE(_sync_key[_tx]);
	if (used > _cap) {
		_broken = true; // the peer wrote an impossible position
		return 0;
	}
	return _cap - used;
}

void datapath::detail::shm::ring_pair::wake_reader()
{
	auto& tx = _hdr->ring[_tx];
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (tx.reader_waiting.load(std::memory_order_relaxed) && tx.reader_waiting.exchange(0))
		_signal(_signal_ctx, _tx * 2 + sig_data);
}

bool datapath::detail::shm::ring_pair::put(cursor& c, uint64_t len, bool more)
{
	const uint8_t* src  = c.ptr();
	uint64_t       need = 8 + align8(len);
	uint64_t       pos  = _head & (_cap - 1);
	uint64_t       gap  = gap_for(pos, src, len);
	uint64_t       f    = free_space();
	uint64_t       total;
	if (!_mirror && _cap - pos < gap + need)
		total = (_cap - pos) + gap_for(0, src, len) + need;
	else
		total = gap + need;
	if (_broken || f < total)
		return false;
	uint8_t* d = _data[_tx];
	if (!_mirror && _cap - pos < gap + need) {
		uint64_t pad = rec_pad; // to the ring start
		std::memcpy(d + pos, &pad, 8);
		_head += _cap - pos;
		pos = 0;
		gap = gap_for(0, src, len);
	}
	if (gap) {
		uint64_t pad = rec_pad | gap;
		std::memcpy(d + pos, &pad, 8);
		_head += gap;
		pos = _head & (_cap - 1);
	}
	uint64_t h = len | (more ? rec_more : 0);
	std::memcpy(d + pos, &h, 8);
	c.copy(d + pos + 8, len);
	_head += need;
	DATAPATH_TSAN_RELEASE(_sync_key[_tx]);
	_hdr->ring[_tx].head.store(_head, std::memory_order_release);
	wake_reader();
	return true;
}

// Write fragments of the message at `c` (`total` bytes left) while they fit.
// `complete` is set once its last fragment is in the ring. An empty message is
// one empty record, so it is delivered like any other.
bool datapath::detail::shm::ring_pair::write_message(cursor& c, uint64_t total, bool& complete, size_t& written)
{
	uint64_t frag_max = (_mirror ? _cap - 8 : _cap / 2 - 8) - page;
	written           = 0;
	complete          = false;
	do {
		uint64_t n = std::min<uint64_t>(total - written, frag_max);
		if (!put(c, n, written + n < total))
			break;
		written += size_t(n);
		complete = written == total;
	} while (!complete);
	return !_broken;
}

datapath::error datapath::detail::shm::ring_pair::send(const buffer_view* parts, size_t count, bool& need_space)
{
	uint64_t total = 0;
	for (size_t i = 0; i < count; i++)
		total += parts[i].size;
	if (total > _limit)
		return datapath::error::TooLarge;
	std::lock_guard<std::mutex> l(_send_lock);
	need_space = false;
	if (_broken)
		return datapath::error::Closed;
	cursor c{parts, count};
	size_t written  = 0;
	bool   complete = false;
	if (_pending.empty()) {
		if (!write_message(c, total, complete, written))
			return datapath::error::Closed;
		if (complete)
			return datapath::error::Success;
	} else if (_pending_bytes + total > _max_queued) {
		return datapath::error::WouldBlock;
	}
	if (written == 0 && _pending_bytes + total > _max_queued)
		return datapath::error::WouldBlock;
	// Keep the rest (a started message must be finished whatever the limit).
	pending p;
	p.bytes.resize(size_t(total - written));
	c.copy(p.bytes.data(), total - written);
	_pending_bytes += p.bytes.size();
	_pending.push_back(std::move(p));
	need_space = true;
	return datapath::error::Success;
}

bool datapath::detail::shm::ring_pair::drain(bool& need_space)
{
	std::lock_guard<std::mutex> l(_send_lock);
	while (!_pending.empty() && !_broken) {
		auto&       p = _pending.front();
		buffer_view v{p.bytes.data() + p.off, p.bytes.size() - p.off};
		cursor      c{&v, 1};
		size_t      written  = 0;
		bool        complete = false;
		write_message(c, v.size, complete, written);
		p.off += written;
		_pending_bytes -= written;
		if (!complete)
			break;
		_pending.pop_front();
	}
	need_space = !_pending.empty() && !_broken;
	return !_broken;
}

bool datapath::detail::shm::ring_pair::prepare_space_wait()
{
	auto& tx = _hdr->ring[_tx];
	tx.writer_waiting.store(1, std::memory_order_seq_cst);
	bool need = false;
	drain(need); // the reader may have made room before it saw the flag
	if (!need)
		tx.writer_waiting.store(0, std::memory_order_relaxed);
	return need;
}

// ---------------------------------------------------------------- reader

bool datapath::detail::shm::ring_pair::prepare_block()
{
	auto& rx = _hdr->ring[_rx];
	rx.reader_waiting.store(1, std::memory_order_seq_cst);
	if (rx.head.load(std::memory_order_seq_cst) != _tail) {
		rx.reader_waiting.store(0, std::memory_order_relaxed);
		return false;
	}
	return true;
}

int datapath::detail::shm::ring_pair::poll_impl(bool (*deliver)(void*, const uint8_t*, size_t), void* ctx)
{
	auto& rx = _hdr->ring[_rx];
	if (rx.reader_waiting.load(std::memory_order_relaxed))
		rx.reader_waiting.store(0, std::memory_order_relaxed);
	uint64_t h = rx.head.load(std::memory_order_acquire);
	DATAPATH_TSAN_ACQUIRE(_sync_key[_rx]);
	if (h == _tail)
		return 0;
	if (h - _tail > _cap)
		return -1;
	uint64_t       frag_max = (_mirror ? _cap - 8 : _cap / 2 - 8) - page;
	const uint8_t* d        = _data[_rx];
	while (_tail != h) {
		uint64_t pos = _tail & (_cap - 1);
		uint64_t rec;
		std::memcpy(&rec, d + pos, 8); // read once: the peer may change it
		uint64_t step;
		bool     stop = false;
		if (rec & rec_pad) {
			uint64_t gap = rec & 0xffffffffu;
			bool     ok;
			if (gap) {
				step = gap;
				ok   = gap % 8 == 0 && gap < page && (_mirror || step <= _cap - pos);
			} else {
				step = _cap - pos;
				ok   = !_mirror && pos != 0;
			}
			if (!ok || step > h - _tail)
				return -1;
		} else {
			uint64_t len  = rec & 0xffffffffu;
			bool     more = (rec & rec_more) != 0;
			step          = 8 + align8(len);
			if (step > h - _tail || len > frag_max || (!_mirror && step > _cap - pos))
				return -1;
			const uint8_t* p = d + pos + 8;
			if (more || !_assembly.empty()) {
				if (_assembly.size() + len > _limit)
					return -1;
				_assembly.insert(_assembly.end(), p, p + len);
				if (!more) {
					stop = !deliver(ctx, _assembly.data(), _assembly.size());
					_assembly.clear();
					if (_assembly.capacity() > (size_t(4) << 20))
						_assembly.shrink_to_fit();
				}
			} else {
				if (len > _limit)
					return -1;
				if (_copy.load(std::memory_order_relaxed)) {
					_scratch.assign(p, p + len);
					p = _scratch.data();
				}
				stop = !deliver(ctx, p, size_t(len));
			}
		}
		_tail += step;
		DATAPATH_TSAN_RELEASE(_sync_key[_rx]);
		rx.tail.store(_tail, std::memory_order_release);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (rx.writer_waiting.load(std::memory_order_relaxed) && rx.writer_waiting.exchange(0))
			_signal(_signal_ctx, _rx * 2 + sig_space);
		if (stop)
			break;
	}
	return 1;
}
