/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Shared-memory data path for a pipe connection (options::shared_memory), the
// Windows half: the section, the events and their wait completion packets. The
// rings themselves are common/shm_ring.hpp.
//
// The named pipe carries the handshake and then stays open only as the
// liveness channel: when either process ends, its pipe handle closes and the
// other side's pending one-byte read fails.
//
// Objects: the server creates (never opens) a section and four auto-reset
// events named "Global\dpshm-<32 hex>-<suffix>" (or "Local\..." when it may not
// create global objects). The name is 128 random bits. Each object's DACL grants
// full access to the server's user and only map/signal rights to the client's
// user, which the server reads from the pipe client's token at identification
// level. Nobody else can open them. Each ring is mapped twice back to back where
// the OS allows it (VirtualAlloc2 placeholders).
//
// The events are tied to the context's completion port with wait completion
// packets, so the I/O threads block in GetQueuedCompletionStatusEx as usual.

#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <datapath/socket.hpp>
#include "common/shm_ring.hpp"
#include "win.hpp"

namespace datapath::windows {
	constexpr size_t shm_offer_size = datapath::detail::shm::offer_size;

	// Arm a wait completion packet: `ov` is queued to `port` once `event` is
	// signalled (right away if it already is). One-shot.
	bool shm_arm_packet(HANDLE packet, HANDLE port, HANDLE event, OVERLAPPED* ov) noexcept;

	class shm_channel {
		public:
		struct objects;

		private:
		std::unique_ptr<objects>                           _o;
		std::unique_ptr<datapath::detail::shm::ring_pair> _ring;

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

		// See datapath::detail::shm::ring_pair.
		datapath::error send(const buffer_view* parts, size_t count, bool& need_space)
		{
			return _ring->send(parts, count, need_space);
		}
		bool drain(bool& need_space)
		{
			return _ring->drain(need_space);
		}
		bool prepare_space_wait()
		{
			return _ring->prepare_space_wait();
		}
		template<typename F>
		int poll(F&& deliver)
		{
			return _ring->poll(std::forward<F>(deliver));
		}
		bool prepare_block()
		{
			return _ring->prepare_block();
		}

		// Wait completion packets and the events they wait on.
		HANDLE data_packet() const noexcept;
		HANDLE space_packet() const noexcept;
		HANDLE rx_data_event() const noexcept;
		HANDLE tx_space_event() const noexcept;
		// Signal both events this side waits on, so armed packets fire (close).
		void wake_self() noexcept;
	};
} // namespace datapath::windows
