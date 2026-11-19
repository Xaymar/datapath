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

// Platform-neutral part of a stream connection: handler storage, framing,
// send queue, and the close-exactly-once state machine.
//
// Threading contract with the platform backend:
// - Messages are delivered only by the backend's "read chain", which runs on
//   I/O threads and is never concurrent with itself for one connection.
// - finish_close() is called either by the read chain (once it observes the
//   connection closed) or, when reads were never armed, by a posted close event.
//   on_close therefore never races with on_message.

#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "detail.hpp"
#include "framing.hpp"
#include "send_queue.hpp"

namespace datapath::detail {
	class connection_base : public connection, public std::enable_shared_from_this<connection_base> {
		std::mutex                       _handler_lock;
		// Held while user callbacks run; release() takes it to wait them out.
		std::recursive_mutex             _callback_lock;
		std::shared_ptr<message_handler> _on_message;
		std::shared_ptr<close_handler>   _on_close;
		std::atomic<bool>                _armed{false};
		std::atomic<bool>                _close_fired{false};
		std::atomic<int32_t>             _reason{int32_t(datapath::error::Closed)};

		protected:
		std::atomic<bool> _closed{false};
		frame_parser      _parser;
		send_queue        _queue;

		connection_base(const datapath::options& opts, uint64_t negotiated_limit)
			: _parser(negotiated_limit, opts.receive_buffer), _queue(negotiated_limit, opts.max_queued_bytes)
		{}

		// Release the OS handle so pending operations fail. Called once.
		virtual void platform_close() = 0;
		// Start the read chain on an I/O thread. Called once.
		virtual void platform_arm() = 0;
		// Arrange for finish_close() to run on an I/O thread (reads never armed).
		virtual void platform_post_close() = 0;
		// The calling thread holds the send queue's writer slot: write until empty
		// or until the OS reports pending.
		virtual void platform_write() = 0;
		// Called after send() wrote data from the caller's thread.
		virtual void after_inline_write() {}
		// Called after on_close has fired (listener bookkeeping).
		virtual void on_finished() {}

		bool armed() const noexcept
		{
			return _armed.load(std::memory_order_acquire);
		}

		// Deliver one message; returns false when the connection got closed.
		bool deliver(const uint8_t* data, size_t size)
		{
			std::shared_ptr<message_handler> h;
			{
				std::lock_guard<std::mutex> l(_handler_lock);
				h = _on_message;
			}
			if (h && *h)
				(*h)(data, size);
			return !_closed.load(std::memory_order_acquire);
		}

		// Feed `n` freshly read bytes to the parser. Returns false when the read
		// chain must stop (and has called finish_close()).
		bool consume(size_t n)
		{
			std::lock_guard<std::recursive_mutex> cb(_callback_lock);
			auto ec = _parser.commit(n, [this](const uint8_t* d, size_t s) { return deliver(d, s); });
			if (ec == datapath::error::Success && !_closed.load(std::memory_order_acquire))
				return true;
			if (ec != datapath::error::Success && ec != datapath::error::Closed)
				close_with(ec);
			finish_close();
			return false;
		}

		// Fire on_close once and drop the handlers (breaks reference cycles).
		void finish_close()
		{
			std::lock_guard<std::recursive_mutex> cb(_callback_lock);
			if (_close_fired.exchange(true))
				return;
			std::shared_ptr<close_handler> h;
			{
				std::lock_guard<std::mutex> l(_handler_lock);
				h = std::move(_on_close);
				_on_message.reset();
			}
			if (h && *h)
				(*h)(datapath::error(_reason.load()));
			on_finished();
		}

		public:
		datapath::error close_with(datapath::error reason)
		{
			if (_closed.exchange(true, std::memory_order_acq_rel))
				return datapath::error::Closed;
			_reason = int32_t(reason);
			_queue.close();
			platform_close();
			if (!_armed.load(std::memory_order_acquire))
				platform_post_close();
			return datapath::error::Success;
		}

		void set_message_handler(message_handler handler) override
		{
			bool has = bool(handler);
			{
				std::lock_guard<std::mutex> l(_handler_lock);
				if (_close_fired)
					return;
				_on_message = std::make_shared<message_handler>(std::move(handler));
			}
			if (has && !_armed.exchange(true, std::memory_order_acq_rel))
				platform_arm();
		}

		void set_close_handler(close_handler handler) override
		{
			std::lock_guard<std::mutex> l(_handler_lock);
			if (_close_fired)
				return;
			_on_close = std::make_shared<close_handler>(std::move(handler));
		}

		datapath::error send(const buffer_view* parts, size_t count) override
		{
			if (_closed.load(std::memory_order_acquire))
				return datapath::error::Closed;
			bool writer = false;
			auto ec     = _queue.push(parts, count, writer);
			if (ec == datapath::error::Success && writer) {
				platform_write();
				after_inline_write();
			}
			return ec;
		}

		datapath::error close() override
		{
			return close_with(datapath::error::Closed);
		}

		void release() override
		{
			close_with(datapath::error::Closed);
			std::lock_guard<std::recursive_mutex> cb(_callback_lock);
			if (_close_fired.exchange(true))
				return;
			{
				std::lock_guard<std::mutex> l(_handler_lock);
				_on_close.reset();
				_on_message.reset();
			}
			on_finished();
		}

		bool good() const override
		{
			return !_closed.load(std::memory_order_acquire);
		}

		uint64_t max_message_size() const override
		{
			return _queue.limit();
		}
	};
} // namespace datapath::detail
