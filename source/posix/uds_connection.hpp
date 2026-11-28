/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "common/compat.hpp"
#include "common/connection_base.hpp"
#include "common/handshake.hpp"
#include "event_context.hpp"
#include "shm.hpp"

namespace datapath { namespace posix {
	class uds_listener;

	class uds_connection : public datapath::detail::connection_base, public fd_target, public poll_target {
		enum post_kind : int { k_arm = 1, k_closed, k_resume, k_drain };

		std::shared_ptr<event_context> _ctx;
		event_loop&                    _loop;
		mutable datapath::detail::shared_mutex _fd_lock;
		int                            _fd;

		std::mutex _mask_lock;
		uint32_t   _mask;

		std::atomic<bool>    _write_parked{false};
		std::vector<uint8_t> _wbatch;
		size_t               _woff = 0;

		// Server-side handshake state (loop thread only).
		bool    _handshaking = false;
		uint8_t _hello[datapath::detail::hello_size + datapath::detail::ext_trailer_size] = {};
		size_t  _hello_got  = 0;
		size_t  _hello_need = datapath::detail::hello_size;
		bool    _ext        = false; // client sent a version 3 hello
		uint32_t _ext_accepted = 0;
		uint8_t  _offer[shm_offer_size] = {};
		int      _offer_fds[shm_fd_count] = {-1, -1, -1, -1, -1}; // owned by _shm
		uint64_t                    _own_limit                           = 0;
		std::weak_ptr<uds_listener> _listener;
		bool                        _close_when_flushed = false; // writer-slot holder only

		// Shared-memory data path (options::shared_memory). Set before the
		// connection is offered or returned and never changed after; active once
		// the server's reply is written (client: from the start). The socket then
		// only tells when the peer is gone.
		struct waker;
		std::unique_ptr<shm_link> _shm;
		std::atomic<bool>         _shm_ready{false};  // the writer slot writes to the ring
		std::atomic<bool>         _shm_active{false}; // send() writes to the ring itself
		bool                      _shm_reading = false; // loop thread only
		bool                      _shm_gone    = false; // loop thread only: peer gone while paused
		bool                      _shm_allowed;
		size_t                    _shm_size;
		uint64_t                  _max_queued;
		std::shared_ptr<waker>    _data_waker, _space_waker;
		std::atomic_flag          _polling = ATOMIC_FLAG_INIT;

		void activate_shm();
		void flush_queue_to_shm();
		void start_shm_reading();
		void shm_peer_gone();
		bool add_waker(bool space);
		void on_wake(bool space, int fd);
		bool send_reply_with_fds();

		std::shared_ptr<uds_connection> self()
		{
			return std::static_pointer_cast<uds_connection>(shared_from_this());
		}

		void update_mask(uint32_t add, uint32_t remove);
		void read_loop();
		void write_loop();
		void hello_read();

		protected:
		void platform_close() override;
		void platform_arm() override;
		void platform_post_close() override;
		void platform_resume() override;
		void platform_post_drain() override;
		void platform_write() override;
		void after_inline_write() override;
		void on_finished() override;
		size_t platform_write_direct(const uint8_t* header, size_t header_size, const buffer_view* parts,
									 size_t count) override;
		bool   direct_write_wanted(uint64_t frame_size) const noexcept override;

		// poll_target
		bool poll() override;
		bool prepare_block() override;

		public:
		uds_connection(std::shared_ptr<event_context> ctx, event_loop& loop, int fd, const datapath::options& opts,
					   uint64_t negotiated_limit);
		~uds_connection() override;

		// Register with the loop. `handshake`: server side, read the client hello first.
		bool start(bool handshake, std::weak_ptr<uds_listener> listener, uint64_t own_limit);
		// Server side, after on_accept returned true.
		void complete_accept();
		// Client side, before start(): messages move through `shm` from now on.
		void use_shared_memory(std::unique_ptr<shm_link> shm);
		// Whether messages move through shared memory (tests).
		bool shared_memory_active() const noexcept
		{
			return _shm_active.load(std::memory_order_acquire);
		}
		datapath::error send(const buffer_view* parts, size_t count) override;

		void on_event(uint32_t events) override;
		void on_post(int kind) override;
	};
}} // namespace datapath::posix
