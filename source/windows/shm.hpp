/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Shared-memory data path for a pipe connection (options::shared_memory).
//
// The named pipe carries the handshake and then stays open only as the
// liveness channel: when either process ends, its pipe handle closes and the
// other side's pending one-byte read fails. Messages move through two
// single-producer/single-consumer rings in one section, one per direction.
//
// Objects: the server creates (never opens) a section and four auto-reset
// events named "Global\dpshm-<32 hex>-<suffix>" (or "Local\..." when it may not
// create global objects). The name is 128 random bits. Each object's DACL grants
// full access to the server's user and only map/signal rights to the client's
// user, which the server reads from the pipe client's token at identification
// level. Nobody else can open them.
//
// Nothing the peer writes into shared memory is trusted: each side keeps its own
// ring position, checks every record against the ring bounds and the negotiated
// message limit, and copies a message out before delivery when the peer is
// another user (so it can't change the bytes while the handler reads them).
//
// Records: uint64 header (low 32 bits payload length, rec_more = more fragments
// follow, rec_pad = padding) then the payload, padded to 8 bytes. Each ring is
// mapped twice back to back where the OS allows it (VirtualAlloc2 placeholders),
// so records never wrap; otherwise a padding record skips to the ring start.
// Records of 4 KiB and more are placed so that the payload sits at the same
// offset within its page as the sender's buffer: with plain 8-byte steps some
// offsets copied 3-5x slower (on Irkela, two round trips in every 512 at
// 256 KiB-1 MiB, or every 16 KiB message in some processes).
//
// Waking: a receiver that has spun for its budget sets reader_waiting and only
// then blocks; a writer calls SetEvent only when it sees that flag. A full ring
// works the same way with writer_waiting. The events are tied to the context's
// completion port with wait completion packets, so the I/O threads block in
// GetQueuedCompletionStatusEx as usual.

#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>
#include <datapath/socket.hpp>
#include "win.hpp"

namespace datapath::windows {
	constexpr size_t shm_offer_size = 64;

	// Arm a wait completion packet: `ov` is queued to `port` once `event` is
	// signalled (right away if it already is). One-shot.
	bool shm_arm_packet(HANDLE packet, HANDLE port, HANDLE event, OVERLAPPED* ov) noexcept;

	class shm_channel {
		public:
		struct objects;

		private:
		std::unique_ptr<objects> _o;
		uint8_t*                 _data[2] = {};
		uint64_t                 _cap     = 0;
		bool                     _mirror  = false;
		bool                     _copy    = true;
		int                      _tx = 0, _rx = 1;
		uint64_t                 _limit;
		uint64_t                 _max_queued;

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

		// Reader side (one poller at a time).
		uint64_t             _tail = 0;
		std::vector<uint8_t> _assembly, _scratch;

		struct cursor;
		uint64_t free_space();
		bool     put(cursor& c, uint64_t len, bool more);
		bool     write_message(cursor& c, uint64_t total, bool may_wait_partial, size_t& written);
		void     wake_reader();

		shm_channel(std::unique_ptr<objects> o, bool server, uint64_t limit, uint64_t max_queued);

		public:
		~shm_channel();
		shm_channel(const shm_channel&)            = delete;
		shm_channel& operator=(const shm_channel&) = delete;

		// Whether this system can run the data path at all (wait completion packets).
		static bool supported() noexcept;

		// Server: create the objects for the client of `pipe` and fill `offer`.
		static datapath::error create(std::unique_ptr<shm_channel>& out, HANDLE pipe, uint64_t ring_size,
									  uint64_t limit, uint64_t max_queued, uint8_t (&offer)[shm_offer_size]);
		// Client: open what the server offered.
		static datapath::error open(std::unique_ptr<shm_channel>& out, const uint8_t (&offer)[shm_offer_size],
									uint64_t limit, uint64_t max_queued);

		// Queue one message: straight into the ring when it fits and nothing
		// waits, else into the pending list (WouldBlock past max_queued).
		// `need_space` is set when pending data waits for the reader.
		datapath::error send(const buffer_view* parts, size_t count, bool& need_space);
		// Move pending data into the ring. Returns false on a broken ring.
		bool drain(bool& need_space);
		// Before waiting for space: set writer_waiting and recheck. True = wait.
		bool prepare_space_wait();

		// Deliver everything the peer published. Returns 1 if there was work, 0 if
		// not, -1 when the peer broke the protocol. `deliver` returns false to stop.
		template<typename F>
		int poll(F&& deliver);
		// Before blocking: set reader_waiting and recheck. True = may block.
		bool prepare_block();

		// Wait completion packets and the events they wait on.
		HANDLE data_packet() const noexcept;
		HANDLE space_packet() const noexcept;
		HANDLE rx_data_event() const noexcept;
		HANDLE tx_space_event() const noexcept;
		// Signal both events this side waits on, so armed packets fire (close).
		void wake_self() noexcept;

		private:
		int poll_impl(bool (*deliver)(void*, const uint8_t*, size_t), void* ctx);
	};

	template<typename F>
	int shm_channel::poll(F&& deliver)
	{
		using fn = std::remove_reference_t<F>;
		return poll_impl([](void* c, const uint8_t* d, size_t n) -> bool { return (*static_cast<fn*>(c))(d, n); },
						 const_cast<void*>(static_cast<const void*>(&deliver)));
	}
} // namespace datapath::windows
