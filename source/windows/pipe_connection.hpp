/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include "common/compat.hpp"
#include "common/connection_base.hpp"
#include "common/handshake.hpp"
#include "iocp_context.hpp"
#include "shm.hpp"

namespace datapath { namespace windows {
	class pipe_listener;

	class pipe_connection : public datapath::detail::connection_base, public io_target, public poll_target {
		enum op_kind : int { k_read = 1, k_write, k_arm, k_close, k_hello, k_wake, k_space };
		enum class io_result { done, pending, failed };

		std::shared_ptr<iocp_context> _ctx;
		mutable datapath::detail::shared_mutex _handle_lock;
		HANDLE                        _h;
		bool                          _skip_on_success;

		io_op _rop, _wop, _aop, _cop, _hop;

		// Writer-slot state.
		std::vector<uint8_t> _wbatch;
		size_t               _woff = 0;
		// Event for direct writes (created on first use, writer-slot holder only).
		// Used with its low bit set so those writes never post to the port.
		HANDLE               _direct_event = nullptr;
		// Outbound pipe buffer size, learned on the first direct send; 0 = unknown.
		std::atomic<uint64_t> _outbound_quota{0};

		// Server-side handshake state.
		uint8_t _hello[datapath::detail::hello_size + datapath::detail::ext_trailer_size] = {};
		size_t  _hello_got  = 0;
		size_t  _hello_need = datapath::detail::hello_size;
		bool    _ext        = false; // client sent a version 3 hello
		uint32_t _ext_accepted = 0;  // version 3: what the reply grants
		uint8_t  _offer[shm_offer_size] = {};
		uint64_t                     _own_limit                           = 0;
		std::weak_ptr<pipe_listener> _listener;
		bool                         _server_side       = false;
		bool                         _close_when_flushed = false; // writer-slot holder only
		std::atomic<bool>            _graceful{false};
		bool                         _pair_requested = false; // client sent a version 2 hello
		// This connection holds the context's core pairing (undone in on_finished).
		std::atomic<bool>            _paired{false};

		// Shared-memory data path (options::shared_memory). Set before the
		// connection is offered or returned and never changed after; active once
		// the server's reply is written (client: from the start). The pipe then
		// only carries a one-byte liveness read.
		std::unique_ptr<shm_channel> _shm;
		std::atomic<bool>            _shm_ready{false};  // the writer slot writes to the ring
		std::atomic<bool>            _shm_active{false}; // send() writes to the ring itself
		bool                         _shm_reading = false; // read chain is the liveness read
		bool                         _shm_allowed;
		bool                         _shm_zero_copy;
		size_t                       _shm_size;
		uint64_t                     _max_queued;
		io_op                        _wake_op, _space_op;
		std::atomic<bool>            _wake_armed{false}, _space_armed{false};
		std::atomic_flag             _polling = ATOMIC_FLAG_INIT;
		uint8_t                      _probe   = 0;

		void start_shm_reading();
		void activate_shm();
		void flush_queue_to_shm();
		void arm_wake();
		void arm_space();

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
		void after_inline_write() override;
		void on_finished() override;
		size_t platform_write_direct(const uint8_t* header, size_t header_size, const buffer_view* parts,
									 size_t count) override;
		bool   direct_write_wanted(uint64_t frame_size) const noexcept override;

		// poll_target
		bool poll() override;
		bool prepare_block() override;

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
		// Client side: messages move through `shm` from now on.
		void use_shared_memory(std::unique_ptr<shm_channel> shm);
		datapath::error send(const buffer_view* parts, size_t count) override;

		// Whether messages move through shared memory (tests).
		bool shared_memory_active() const noexcept
		{
			return _shm_active.load(std::memory_order_acquire);
		}

		// Client side: this connection's context paired its I/O thread for it.
		void set_paired() noexcept
		{
			_paired.store(true, std::memory_order_release);
		}

		void on_io(io_op* op, DWORD bytes, DWORD err) override;
	};
}} // namespace datapath::windows
