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

#pragma once
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include "common/detail.hpp"
#include "epoll_context.hpp"
#include "path.hpp"

namespace datapath::linux_ {
	class uds_connection;

	class uds_listener : public datapath::detail::listener,
						 public fd_target,
						 public std::enable_shared_from_this<uds_listener> {
		enum post_kind : int { k_offer = 1 };

		std::shared_ptr<epoll_context> _ctx;
		event_loop&                    _loop;
		socket_address                 _addr;
		datapath::options              _opts;
		int                            _fd = -1;
		bool                           _unlink_on_close = false;

		std::mutex                                  _lock;
		std::recursive_mutex                        _callback_lock;
		bool                                        _closed    = false;
		size_t                                      _connected = 0;
		datapath::detail::accept_fn                 _handler;
		std::deque<std::shared_ptr<uds_connection>> _waiting;
		std::vector<std::weak_ptr<uds_connection>>  _conns;
		bool                                        _offer_posted = false;

		bool peer_allowed(int fd) const;

		public:
		uds_listener(std::shared_ptr<epoll_context> ctx, event_loop& loop, socket_address addr,
					 const datapath::options& opts);
		~uds_listener() override;

		datapath::error create();

		void offer(std::shared_ptr<uds_connection> conn);
		void connection_finished();

		void            set_accept_handler(datapath::detail::accept_fn handler) override;
		datapath::error close() override;
		void            release() override;

		void on_event(uint32_t events) override;
		void on_post(int kind) override;
	};
} // namespace datapath::linux_
