/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// The platform-neutral half of the shared-memory data path
// (options::shared_memory): two single-producer/single-consumer byte rings in
// one shared section, one per direction. The platform code (windows/shm.cpp,
// posix/shm.cpp) creates and maps the section and owns the wake-up objects;
// this class only reads and writes the rings and asks the platform to signal.
//
// Nothing the peer writes into shared memory is trusted: each side keeps its own
// ring position, checks every record against the ring bounds and the negotiated
// message limit, and copies a message out before delivery when the peer is
// another user (so it can't change the bytes while the handler reads them).
//
// Records: uint64 header (low 32 bits payload length, rec_more = more fragments
// follow, rec_pad = padding) then the payload, padded to 8 bytes. Rings mapped
// twice back to back ("mirrored") never wrap a record; otherwise a padding
// record skips to the ring start. Records of 4 KiB and more are placed so that
// the payload sits at the same offset within its page as the sender's buffer:
// with plain 8-byte steps some offsets copied 3-5x slower (on Irkela, two round
// trips in every 512 at 256 KiB-1 MiB, or every 16 KiB message in some
// processes).
//
// Waking: a receiver that has spun for its budget sets reader_waiting and only
// then blocks; a writer signals only when it sees that flag. A full ring works
// the same way with writer_waiting.

#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <type_traits>
#include <vector>
#include <datapath/socket.hpp>
#include "compat.hpp"

namespace datapath { namespace detail { namespace shm {
	constexpr uint32_t magic       = 0x4d535044; // "DPSM"
	constexpr uint32_t version     = 1;
	constexpr uint64_t min_ring    = uint64_t(64) << 10;
	constexpr uint64_t max_ring    = uint64_t(1) << 30;
	constexpr uint32_t flag_mirror = 1;
	// Rings start on a 64 KiB boundary so each can be mapped on its own
	// (Windows allocation granularity, and any page size up to 64 KiB).
	constexpr uint64_t data_offset = 64 * 1024;
	constexpr size_t   offer_size  = 64;

	struct alignas(64) ring_ctl {
		alignas(64) std::atomic<uint64_t> head;           // bytes published by the writer
		alignas(64) std::atomic<uint64_t> tail;           // bytes released by the reader
		alignas(64) std::atomic<uint32_t> reader_waiting; // reader is (about to be) blocked
		alignas(64) std::atomic<uint32_t> writer_waiting; // writer is (about to be) blocked for space
	};

	struct section_header {
		uint32_t magic;
		uint32_t version;
		uint64_t capacity;
		uint32_t flags;
		ring_ctl ring[2]; // [0] server -> client, [1] client -> server
	};
	static_assert(DATAPATH_ATOMIC_ALWAYS_LOCK_FREE(uint64_t), "shared atomics must be lock-free");
	static_assert(DATAPATH_ATOMIC_ALWAYS_LOCK_FREE(uint32_t), "shared atomics must be lock-free");
	static_assert(sizeof(section_header) <= data_offset, "header must fit before the rings");

	// Ring size for a requested size: a power of two in [min_ring, max_ring].
	uint64_t ring_capacity(uint64_t requested) noexcept;
	// Initialize a fresh header at `view`.
	section_header* init_header(void* view, uint64_t cap, uint32_t flags) noexcept;
	// Whether the header at `view` matches what the offer announced.
	bool header_matches(const void* view, uint64_t cap, uint32_t flags) noexcept;

	// Which wake-up the ring asks the platform for. Index = direction * 2 + kind.
	enum signal_kind : int { sig_data = 0, sig_space = 1 };

	class ring_pair {
		public:
		using signal_fn = void (*)(void* ctx, int index);

		struct setup {
			section_header* header;
			uint8_t*        data[2]; // ring 0, ring 1 (mirrored: each mapped twice)
			uint64_t        cap;
			bool            mirrored;
			bool            copy; // copy messages out before delivery
			bool            server;
			uint64_t        limit;
			uint64_t        max_queued;
			signal_fn       signal;
			void*           signal_ctx;
			// Optional: one address per ring that is the same at both ends of
			// the section in this process (ThreadSanitizer only).
			const void* sync_key[2];
		};

		private:
		section_header* _hdr;
		uint8_t*        _data[2];
		uint64_t        _cap;
		bool            _mirror;
		bool            _copy;
		int             _tx, _rx;
		uint64_t        _limit;
		uint64_t        _max_queued;
		signal_fn       _signal;
		void*           _signal_ctx;
		const void*     _sync_key[2];

		// Writer side, under _send_lock.
		std::mutex _send_lock;
		uint64_t   _head = 0; // own copy, never re-read from shared memory
		struct pending {
			std::vector<uint8_t> bytes;
			size_t               off = 0;
		};
		std::deque<pending> _pending;
		uint64_t            _pending_bytes = 0;
		bool                _broken        = false;
		bool                _blocked       = false; // send() returned WouldBlock since pending ran empty

		// Reader side (one poller at a time).
		uint64_t             _tail = 0;
		std::vector<uint8_t> _assembly, _scratch;

		struct cursor;
		uint64_t free_space();
		bool     put(cursor& c, uint64_t len, bool more);
		bool     write_message(cursor& c, uint64_t total, bool& complete, size_t& written);
		void     wake_reader();
		int      poll_impl(bool (*deliver)(void*, const uint8_t*, size_t), void* ctx);

		public:
		explicit ring_pair(const setup& s);
		ring_pair(const ring_pair&)            = delete;
		ring_pair& operator=(const ring_pair&) = delete;

		int tx() const noexcept
		{
			return _tx;
		}
		int rx() const noexcept
		{
			return _rx;
		}

		// Queue one message: straight into the ring when it fits and nothing
		// waits, else into the pending list (WouldBlock past max_queued).
		// `need_space` is set when pending data waits for the reader.
		datapath::error send(const buffer_view* parts, size_t count, bool& need_space);
		// Move pending data into the ring. Returns false on a broken ring.
		bool drain(bool& need_space);
		// Before waiting for space: set writer_waiting and recheck. True = wait.
		bool prepare_space_wait();
		// True once after send() returned WouldBlock and the pending list has
		// since been moved into the ring completely.
		bool take_drained();

		// Deliver everything the peer published. Returns 1 if there was work, 0 if
		// not, -1 when the peer broke the protocol. `deliver` returns false to stop.
		template<typename F>
		int poll(F&& deliver)
		{
			using fn = typename std::remove_reference<F>::type;
			return poll_impl([](void* c, const uint8_t* d, size_t n) -> bool { return (*static_cast<fn*>(c))(d, n); },
							 const_cast<void*>(static_cast<const void*>(&deliver)));
		}
		// Before blocking: set reader_waiting and recheck. True = may block.
		bool prepare_block();
	};
}}} // namespace datapath::detail::shm
