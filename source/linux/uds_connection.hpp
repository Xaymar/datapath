/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include "common/connection_base.hpp"
#include "common/handshake.hpp"
#include "epoll_context.hpp"

namespace datapath::linux_ {
	class uds_listener;

	class uds_connection : public datapath::detail::connection_base, public fd_target {
		enum post_kind : int { k_arm = 1, k_closed };

		std::shared_ptr<epoll_context> _ctx;
		event_loop&                    _loop;
		mutable std::shared_mutex      _fd_lock;
		int                            _fd;

		std::mutex _mask_lock;
		uint32_t   _mask;

		std::atomic<bool>    _write_parked{false};
		std::vector<uint8_t> _wbatch;
		size_t               _woff = 0;

		// Server-side handshake state (loop thread only).
		bool                        _handshaking = false;
		uint8_t                     _hello[datapath::detail::hello_size] = {};
		size_t                      _hello_got                           = 0;
		uint64_t                    _own_limit                           = 0;
		std::weak_ptr<uds_listener> _listener;
		bool                        _close_when_flushed = false; // writer-slot holder only

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
		void platform_write() override;
		void after_inline_write() override;
		void on_finished() override;

		public:
		uds_connection(std::shared_ptr<epoll_context> ctx, event_loop& loop, int fd, const datapath::options& opts,
					   uint64_t negotiated_limit);
		~uds_connection() override;

		// Register with the loop. `handshake`: server side, read the client hello first.
		bool start(bool handshake, std::weak_ptr<uds_listener> listener, uint64_t own_limit);
		// Server side, after on_accept returned true.
		void complete_accept();

		void on_event(uint32_t events) override;
		void on_post(int kind) override;
	};
} // namespace datapath::linux_
