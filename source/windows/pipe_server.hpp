/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/detail.hpp"
#include "iocp_context.hpp"

namespace datapath::windows {
	class pipe_connection;

	class pipe_listener : public datapath::detail::listener,
						  public io_target,
						  public std::enable_shared_from_this<pipe_listener> {
		struct instance {
			io_op  op;
			HANDLE h    = INVALID_HANDLE_VALUE;
			bool   skip = false;
		};
		enum op_kind : int { k_connect = 1, k_offer };

		std::shared_ptr<iocp_context> _ctx;
		std::wstring                  _path;
		datapath::options             _opts;
		PSECURITY_DESCRIPTOR          _sd = nullptr;
		SECURITY_ATTRIBUTES           _sa{};

		std::mutex                                    _lock;
		std::recursive_mutex                          _callback_lock;
		std::vector<std::unique_ptr<instance>>        _pending;
		size_t                                        _connected = 0;
		bool                                          _closed    = false;
		datapath::detail::accept_fn                   _handler;
		std::deque<std::shared_ptr<pipe_connection>>  _waiting;
		std::vector<std::weak_ptr<pipe_connection>>   _conns;
		io_op                                         _offer_op;
		bool                                          _offer_posted = false;

		datapath::error spawn_locked(bool first);
		void            refill_locked();

		public:
		pipe_listener(std::shared_ptr<iocp_context> ctx, std::wstring path, const datapath::options& opts);
		~pipe_listener() override;

		datapath::error create();

		// From pipe_connection: handshake passed, decide acceptance.
		void offer(std::shared_ptr<pipe_connection> conn);
		// From pipe_connection: a connection that occupied a client slot ended.
		void connection_finished();

		void            set_accept_handler(datapath::detail::accept_fn handler) override;
		datapath::error close() override;
		void            release() override;
		void            on_io(io_op* op, DWORD bytes, DWORD err) override;
	};
} // namespace datapath::windows
