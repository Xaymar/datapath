/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Shared-memory data path for a Unix socket connection (options::shared_memory),
// the POSIX half. Linux and Android only for now; elsewhere supported() is false
// and connections stay on the socket. The rings are common/shm_ring.hpp.
//
// The socket carries the handshake and then stays open only as the liveness
// channel: when either process ends, the other side sees the hang-up.
//
// Objects: the server creates a memfd for the section and four eventfds (data
// and space for each direction) and passes them to the client with SCM_RIGHTS
// along with its reply. They have no name, so nobody else can open them. The
// memfd is sealed against shrinking and growing before it is sent, and the
// client checks the seals: otherwise the peer could truncate it and make every
// access to the mapping raise SIGBUS. Both rings are always mapped twice back to
// back (mirrored).
//
// The eventfds are registered with the connection's event loop, so an idle I/O
// thread blocks in epoll_wait as usual.

#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <datapath/options.hpp>
#include <datapath/socket.hpp>
#include "common/shm_ring.hpp"

namespace datapath { namespace posix {
	constexpr size_t shm_offer_size = datapath::detail::shm::offer_size;
	constexpr int    shm_fd_count   = 5; // section, then data/space eventfds of ring 0 and ring 1

	class shm_link {
		public:
		struct objects;

		private:
		std::unique_ptr<objects>                           _o;
		std::unique_ptr<datapath::detail::shm::ring_pair> _ring;

		shm_link(std::unique_ptr<objects> o, bool server, bool copy, uint64_t limit, uint64_t max_queued);

		public:
		~shm_link();
		shm_link(const shm_link&)            = delete;
		shm_link& operator=(const shm_link&) = delete;

		// Whether this system can run the data path at all.
		static bool supported() noexcept;
		// Whether connections with these options should use it
		// (DATAPATH_SHARED_MEMORY=1/0 overrides the option).
		static bool wanted(const datapath::options& opts) noexcept;

		// Server: create the objects for the peer of socket `sock`, fill `offer`
		// and the descriptors to send with it (owned by the link).
		static datapath::error create(std::unique_ptr<shm_link>& out, int sock, uint64_t ring_size, uint64_t limit,
									  uint64_t max_queued, uint8_t (&offer)[shm_offer_size],
									  int (&fds)[shm_fd_count]);
		// Client: open what the server offered. Takes ownership of `fds` (also on
		// failure).
		static datapath::error open(std::unique_ptr<shm_link>& out, int sock, const uint8_t (&offer)[shm_offer_size],
									int (&fds)[shm_fd_count], uint64_t limit, uint64_t max_queued);

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
		bool take_drained()
		{
			return _ring->take_drained();
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

		// The eventfds this side waits on.
		int rx_data_fd() const noexcept;
		int tx_space_fd() const noexcept;
		// Reset an eventfd after it was reported readable.
		static void clear(int efd) noexcept;
	};
}} // namespace datapath::posix
