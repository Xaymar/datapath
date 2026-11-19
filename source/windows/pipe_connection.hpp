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
#include <memory>
#include <shared_mutex>
#include <vector>
#include "common/connection_base.hpp"
#include "common/handshake.hpp"
#include "iocp_context.hpp"

namespace datapath::windows {
	class pipe_listener;

	class pipe_connection : public datapath::detail::connection_base, public io_target {
		enum op_kind : int { k_read = 1, k_write, k_arm, k_close, k_hello };
		enum class io_result { done, pending, failed };

		std::shared_ptr<iocp_context> _ctx;
		mutable std::shared_mutex     _handle_lock;
		HANDLE                        _h;
		bool                          _skip_on_success;

		io_op _rop, _wop, _aop, _cop, _hop;

		// Writer-slot state.
		std::vector<uint8_t> _wbatch;
		size_t               _woff = 0;

		// Server-side handshake state.
		uint8_t                      _hello[datapath::detail::hello_size] = {};
		size_t                       _hello_got                           = 0;
		uint64_t                     _own_limit                           = 0;
		std::weak_ptr<pipe_listener> _listener;
		bool                         _server_side       = false;
		bool                         _close_when_flushed = false; // writer-slot holder only
		std::atomic<bool>            _graceful{false};

		std::shared_ptr<pipe_connection> self()
		{
			return std::static_pointer_cast<pipe_connection>(shared_from_this());
		}

		io_result issue(bool write, const uint8_t* buf, size_t size, io_op& op, DWORD& bytes, DWORD& err);
		void      read_loop();
		void      write_loop();
		void      hello_loop();
		void      hello_done();

		protected:
		void platform_close() override;
		void platform_arm() override;
		void platform_post_close() override;
		void platform_write() override;
		void on_finished() override;

		public:
		pipe_connection(std::shared_ptr<iocp_context> ctx, HANDLE h, bool skip_on_success,
						const datapath::options& opts, uint64_t negotiated_limit);
		~pipe_connection() override;

		// Server side: hold the writer slot, read and validate the client's hello,
		// then offer the connection to the listener.
		void begin_server_handshake(std::weak_ptr<pipe_listener> listener, uint64_t own_limit);
		// Server side, after on_accept returned true: send the reply, then release
		// anything queued meanwhile.
		void complete_accept();

		void on_io(io_op* op, DWORD bytes, DWORD err) override;
	};
} // namespace datapath::windows
