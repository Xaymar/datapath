/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
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
// - pause() stops the read chain at the next message boundary: it delivers no
//   further message and marks itself stalled (stall_if_paused()). resume()
//   takes the stall back and has platform_resume() restart the chain on an I/O
//   thread, which first delivers what is still buffered (resume_buffered()).
//   While stalled, nothing is read, so the peer's sends back up into its own
//   send queue and it gets WouldBlock: backpressure without blocking the I/O
//   thread that other connections share.

#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "detail.hpp"
#include "framing.hpp"
#include "send_queue.hpp"

namespace datapath { namespace detail {
	class connection_base : public connection, public std::enable_shared_from_this<connection_base> {
		std::mutex                       _handler_lock;
		// Held while user callbacks run; release() takes it to wait them out.
		std::recursive_mutex             _callback_lock;
		std::shared_ptr<message_handler> _on_message;
		std::shared_ptr<close_handler>   _on_close;
		std::shared_ptr<drain_handler>   _on_drain;
		std::atomic<bool>                _armed{false};
		std::atomic<bool>                _close_fired{false};
		std::atomic<int32_t>             _reason{int32_t(datapath::error::Closed)};
		// Receiving paused by the user.
		std::atomic<bool>                _paused{false};
		// The read chain stopped because of _paused; whoever clears it restarts it.
		std::atomic<bool>                _stalled{false};
		// Read chain only: parsing stopped for a pause with frames left buffered.
		bool                             _unparsed = false;
		// A drain notification is posted and has not run yet.
		std::atomic<bool>                _drain_posted{false};

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
		// Arrange for the read chain to continue on an I/O thread after a stall.
		// Called once per stall.
		virtual void platform_resume() = 0;
		// Arrange for fire_drain() to run on an I/O thread. Called once per post.
		virtual void platform_post_drain() = 0;
		// Called after send() wrote data from the caller's thread.
		virtual void after_inline_write() {}
		// Direct send, caller holds the writer slot and nothing is queued: write
		// `header` then `parts` straight from these buffers without blocking, and
		// return how many bytes the OS took right away (the rest gets queued).
		// Must not leave the OS holding a pointer into the buffers on return. On a
		// hard error, close the connection and return 0. The default writes nothing.
		virtual size_t platform_write_direct(const uint8_t* header, size_t header_size, const buffer_view* parts,
											 size_t count)
		{
			(void)header, (void)header_size, (void)parts, (void)count;
			return 0;
		}
		// Whether platform_write_direct() is worth trying for a frame of this size.
		virtual bool direct_write_wanted(uint64_t frame_size) const noexcept
		{
			(void)frame_size;
			return false;
		}
		// Called after on_close has fired (listener bookkeeping).
		virtual void on_finished() {}

		bool armed() const noexcept
		{
			return _armed.load(std::memory_order_acquire);
		}

		bool paused() const noexcept
		{
			return _paused.load(std::memory_order_acquire);
		}

		// Read chain: true when it must stop because receiving is paused. It is
		// then stalled and resume() restarts it via platform_resume().
		bool stall_if_paused()
		{
			if (!_paused.load(std::memory_order_seq_cst))
				return false;
			_stalled.store(true, std::memory_order_seq_cst);
			if (_paused.load(std::memory_order_seq_cst))
				return true;
			// resume() ran meanwhile. Unless it already took the stall (and posted
			// a restart), take it back and keep going.
			return !_stalled.exchange(false, std::memory_order_seq_cst);
		}

		// Read chain: frames are left in the receive buffer because parsing stopped
		// for a pause. Deliver them with resume_buffered() before reading more:
		// the OS may have nothing new to report (edge-triggered readiness).
		bool unparsed() const noexcept
		{
			return _unparsed;
		}

		// Read chain: deliver complete frames that were left in the receive buffer
		// when parsing stopped for a pause. Same result as consume().
		bool resume_buffered()
		{
			std::lock_guard<std::recursive_mutex> cb(_callback_lock);
			if (_closed.load(std::memory_order_acquire)) {
				finish_close();
				return false;
			}
			_unparsed = false;
			return parsed(_parser.resume([this](const uint8_t* d, size_t s) { return deliver(d, s); }));
		}

		// After the send queue (or shared-memory pending list) may have run empty:
		// notify on_drain if a send returned WouldBlock before.
		void check_drained()
		{
			if (_queue.take_drained())
				post_drain();
		}

		// Any thread: have fire_drain() run on an I/O thread.
		void post_drain()
		{
			if (!_drain_posted.exchange(true, std::memory_order_acq_rel))
				platform_post_drain();
		}

		// I/O thread: call on_drain.
		void fire_drain()
		{
			_drain_posted.store(false, std::memory_order_release);
			std::lock_guard<std::recursive_mutex> cb(_callback_lock);
			if (_closed.load(std::memory_order_acquire))
				return;
			std::shared_ptr<drain_handler> h;
			{
				std::lock_guard<std::mutex> l(_handler_lock);
				h = _on_drain;
			}
			if (h && *h)
				(*h)();
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
			// Paused (also from inside the handler): stop after this message.
			return !_closed.load(std::memory_order_acquire) && !_paused.load(std::memory_order_acquire);
		}

		// Deliver one message that did not come through the parser (shared memory).
		// Same rules as consume(): only from the read chain. False once closed or
		// paused.
		bool deliver_message(const uint8_t* data, size_t size)
		{
			std::lock_guard<std::recursive_mutex> cb(_callback_lock);
			if (_closed.load(std::memory_order_acquire))
				return false;
			return deliver(data, size);
		}

		// Feed `n` freshly read bytes to the parser. Returns false when the read
		// chain must stop (and has called finish_close()).
		bool consume(size_t n)
		{
			std::lock_guard<std::recursive_mutex> cb(_callback_lock);
			if (_paused.load(std::memory_order_acquire)) {
				// Paused while this read was in flight (always the case for the
				// read Windows keeps pending): keep the bytes, deliver on resume.
				_parser.store(n);
				_unparsed = true;
				return true;
			}
			return parsed(_parser.commit(n, [this](const uint8_t* d, size_t s) { return deliver(d, s); }));
		}

		bool parsed(datapath::error ec)
		{
			// Closed without a close: deliver() stopped because receiving got paused.
			// The rest stays buffered; the chain stalls at its next step.
			if (ec == datapath::error::Closed && !_closed.load(std::memory_order_acquire)) {
				ec        = datapath::error::Success;
				_unparsed = true;
			}
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
				_on_drain.reset();
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
			// Nothing is in flight for a chain that was never armed or that is
			// stalled, so nothing would notice the close: post it.
			if (!_armed.load(std::memory_order_acquire) || _stalled.exchange(false, std::memory_order_seq_cst))
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

		void set_drain_handler(drain_handler handler) override
		{
			std::lock_guard<std::mutex> l(_handler_lock);
			if (_close_fired)
				return;
			_on_drain = std::make_shared<drain_handler>(std::move(handler));
		}

		void pause() override
		{
			_paused.store(true, std::memory_order_seq_cst);
		}

		void resume() override
		{
			_paused.store(false, std::memory_order_seq_cst);
			// The restarted chain notices a close that came meanwhile.
			if (_stalled.exchange(false, std::memory_order_seq_cst))
				platform_resume();
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
			uint64_t total = 0;
			for (size_t i = 0; i < count; i++)
				total += parts[i].size;
			if (direct_write_wanted(total + frame_header_size)) {
				// Large message and nothing queued: write it from the caller's buffers
				// instead of copying it into the queue first. Only what the OS does
				// not take right away gets copied.
				bool direct = false;
				auto ec     = _queue.try_begin_direct(total, direct);
				if (ec != datapath::error::Success)
					return ec;
				if (direct) {
					uint8_t header[frame_header_size];
					encode_frame_header(header, total);
					size_t sent = platform_write_direct(header, frame_header_size, parts, count);
					_queue.push_front_rest(header, frame_header_size, parts, count, sent);
					platform_write(); // drains what others queued meanwhile, then releases the slot
					after_inline_write();
					return datapath::error::Success;
				}
			}
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
				_on_drain.reset();
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
}} // namespace datapath::detail
