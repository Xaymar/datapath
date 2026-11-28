/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Python bindings for DataPath (nanobind, bound to the C++ API directly).
//
// Two ways to receive, chosen per socket/server:
// - Pull (recv()/accept()): the I/O thread copies each message into a queue and
//   never touches the interpreter. The Python thread spins briefly with the GIL
//   released, then sleeps on a condition variable. This keeps the GIL off the
//   I/O thread, so a busy Python thread can't stall the I/O thread (and with it
//   every other connection on the context). The queue is bounded: once it holds
//   recv_high_water_mark bytes (each message counts its size plus 64), the
//   socket stops reading from the peer, whose sends then back up and fail with
//   WouldBlockError; recv() starts reading again once it took the queue below
//   the mark. A peer that sends faster than Python reads can't grow it without
//   bound.
// - Callbacks (on_message()/on_accept()): the I/O thread takes the GIL and calls
//   the Python function directly. One thread hop less, but taking the GIL can
//   wait up to sys.getswitchinterval() while another Python thread runs.
//
// Locking rules that keep this deadlock-free:
// - Anything that may wait for a running handler (destroying a socket, server or
//   context) releases the GIL first, because the handler may be waiting for it.
// - Handlers never touch a wrapper after its Python callback returns: the
//   callback may have dropped the last reference and destroyed the wrapper.
// - At interpreter exit (atexit) handlers stop taking the GIL, and exit waits for
//   the ones already inside Python to return.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <datapath/datapath.hpp>
#include <deque>
#include <memory>
#include <mutex>
#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <optional>
#include <thread>
#include <vector>
#include "common/spin.hpp"

namespace nb = nanobind;
using namespace nb::literals;
using clock_type = std::chrono::steady_clock;

namespace {
	// ---------------------------------------------------------------------------
	// Interpreter lifetime and handler bookkeeping

	std::atomic<bool> g_finalizing{false};
	std::atomic<int>  g_in_python{0};
	thread_local int  t_handler_depth = 0;

	// Entered by every I/O-thread handler that wants the GIL. Order matters:
	// count first, then check the flag, so shutdown() can't miss a handler.
	class python_entry {
		bool _ok;

		public:
		python_entry()
		{
			g_in_python.fetch_add(1, std::memory_order_seq_cst);
			_ok = !g_finalizing.load(std::memory_order_seq_cst);
		}
		~python_entry()
		{
			g_in_python.fetch_sub(1, std::memory_order_seq_cst);
		}
		explicit operator bool() const noexcept
		{
			return _ok;
		}
	};

	struct handler_scope {
		handler_scope()
		{
			++t_handler_depth;
		}
		~handler_scope()
		{
			--t_handler_depth;
		}
	};

	// Called from atexit: stop handlers from entering Python and wait for the
	// ones that are inside it.
	void shutdown()
	{
		g_finalizing.store(true, std::memory_order_seq_cst);
		nb::gil_scoped_release release;
		while (g_in_python.load(std::memory_order_seq_cst) != 0)
			std::this_thread::yield();
	}

	// ---------------------------------------------------------------------------
	// Errors

	PyObject* g_errors = nullptr; // datapath._errors (kept for the process lifetime)

	nb::object error_class(datapath::error ec)
	{
		const char* name = "DatapathError";
		switch (ec) {
		case datapath::error::Closed:
			name = "ClosedError";
			break;
		case datapath::error::TimedOut:
			name = "TimedOutError";
			break;
		case datapath::error::InvalidPath:
			name = "InvalidPathError";
			break;
		case datapath::error::NotSupported:
			name = "NotSupportedError";
			break;
		case datapath::error::TooLarge:
			name = "TooLargeError";
			break;
		case datapath::error::WouldBlock:
			name = "WouldBlockError";
			break;
		case datapath::error::VersionMismatch:
			name = "VersionMismatchError";
			break;
		case datapath::error::NotFound:
			name = "NotFoundError";
			break;
		case datapath::error::AddressInUse:
			name = "AddressInUseError";
			break;
		case datapath::error::AccessDenied:
			name = "AccessDeniedError";
			break;
		default:
			break;
		}
		return nb::handle(g_errors).attr(name);
	}

	[[noreturn]] void raise_error(datapath::error ec, const char* what)
	{
		nb::object cls    = error_class(ec);
		nb::object exc    = cls(nb::str("{}: {}").format(what, datapath::to_string(ec)));
		exc.attr("error") = nb::cast(ec);
		PyErr_SetObject(cls.ptr(), exc.ptr());
		throw nb::python_error();
	}

	inline void check(datapath::error ec, const char* what)
	{
		if (ec != datapath::error::Success)
			raise_error(ec, what);
	}

	// ---------------------------------------------------------------------------
	// Timeouts

	// Wait in slices so Ctrl+C is noticed; a notification still wakes at once.
	constexpr auto signal_slice = std::chrono::milliseconds(50);

	std::optional<clock_type::time_point> deadline_from(std::optional<double> timeout)
	{
		if (!timeout)
			return std::nullopt;
		if (*timeout < 0)
			throw nb::value_error("timeout must be non-negative or None");
		return clock_type::now()
			   + std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(*timeout));
	}

	// Spin (GIL released by the caller) until `ready()` or `spin` passes.
	template<typename Ready>
	bool spin_until(std::chrono::microseconds spin, Ready&& ready)
	{
		if (spin.count() <= 0)
			return ready();
		auto end = clock_type::now() + spin;
		for (;;) {
			if (ready())
				return true;
			if (clock_type::now() >= end)
				return false;
			DATAPATH_CPU_RELAX();
		}
	}

	// Blocking wait shared by Socket.recv() and Server.accept(). `try_take` is
	// called with the GIL released and the queue lock held.
	template<typename Queue, typename TryTake>
	bool wait_queue(Queue& q, std::optional<clock_type::time_point> deadline, std::chrono::microseconds spin,
					TryTake&& try_take)
	{
		bool spun = false;
		for (;;) {
			bool done = false;
			{
				nb::gil_scoped_release release;
				if (!spun) {
					spun = true;
					if (deadline) {
						auto left =
							std::chrono::duration_cast<std::chrono::microseconds>(*deadline - clock_type::now());
						if (left < spin)
							spin = left;
					}
					spin_until(spin, [&] { return q.count.load(std::memory_order_acquire) != 0 || q.closed_flag(); });
				}
				std::unique_lock<std::mutex> l(q.lock);
				auto                         slice_end = clock_type::now() + signal_slice;
				if (deadline && *deadline < slice_end)
					slice_end = *deadline;
				q.waiters++;
				while (!(done = try_take()) && !q.closed) {
					if (q.cv.wait_until(l, slice_end) == std::cv_status::timeout)
						break;
				}
				q.waiters--;
				if (!done && q.closed)
					done = try_take();
				if (!done && q.closed)
					return false;
			}
			if (done)
				return true;
			if (PyErr_CheckSignals() != 0)
				throw nb::python_error();
			if (deadline && clock_type::now() >= *deadline)
				raise_error(datapath::error::TimedOut, "timed out");
		}
	}

	// ---------------------------------------------------------------------------
	// Context

	struct py_context {
		std::shared_ptr<datapath::context> ctx;

		explicit py_context(std::shared_ptr<datapath::context> c) : ctx(std::move(c)) {}
		~py_context()
		{
			close();
		}

		// Destroying a context joins its I/O threads, which must not happen on one
		// of them; hand it to a helper thread in that case.
		void close()
		{
			if (!ctx)
				return;
			auto c = std::move(ctx);
			if (t_handler_depth > 0) {
				std::thread([c = std::move(c)]() mutable { c.reset(); }).detach();
				return;
			}
			nb::gil_scoped_release release;
			c.reset();
		}
	};

	std::shared_ptr<datapath::context> context_of(nb::handle h)
	{
		if (!h.is_valid() || h.is_none())
			return nullptr;
		auto& c = nb::cast<py_context&>(h);
		if (!c.ctx)
			raise_error(datapath::error::Closed, "context is closed");
		return c.ctx;
	}

	// ---------------------------------------------------------------------------
	// Socket

	struct py_socket;

	// accept(), recv() and recv_into() are deprecated in 1.1.0 and go away in 2.0.0
	// in favour of the callbacks, which every other binding uses.
	void warn_deprecated(const char* what)
	{
		std::string msg =
			std::string(what)
			+ " is deprecated and will be removed in DataPath 2.0; use on_accept/on_message callbacks instead";
		if (PyErr_WarnEx(PyExc_DeprecationWarning, msg.c_str(), 1) != 0)
			throw nb::python_error();
	}

	nb::object wrap_socket(std::shared_ptr<datapath::socket> s, nb::handle context);

	struct message_queue {
		std::mutex                        lock;
		std::condition_variable           cv;
		std::deque<std::vector<uint8_t>>  items;
		std::vector<std::vector<uint8_t>> spare; // recycled buffers
		std::atomic<size_t>               count{0};
		// Bytes queued (size + message_overhead each); compared to high_water.
		std::atomic<size_t>               bytes{0};
		std::atomic<bool>                 closed_atomic{false};
		int                               waiters = 0;
		bool                              closed  = false;
		datapath::error                   reason  = datapath::error::Closed;

		static constexpr size_t message_overhead = 64;

		bool closed_flag() const noexcept
		{
			return closed_atomic.load(std::memory_order_acquire);
		}

		// Lock held. Returns true if a waiter needs a wake().
		bool push_locked(const uint8_t* data, size_t size)
		{
			std::vector<uint8_t> buf;
			if (!spare.empty()) {
				buf = std::move(spare.back());
				spare.pop_back();
			}
			buf.assign(data, data + size);
			items.push_back(std::move(buf));
			bytes.fetch_add(size + message_overhead, std::memory_order_acq_rel);
			count.fetch_add(1, std::memory_order_release);
			return waiters > 0;
		}

		// Lock held: take the front item.
		std::vector<uint8_t> pop_locked()
		{
			auto msg = std::move(items.front());
			items.pop_front();
			bytes.fetch_sub(msg.size() + message_overhead, std::memory_order_acq_rel);
			count.fetch_sub(1, std::memory_order_relaxed);
			return msg;
		}

		void wake()
		{
			cv.notify_one();
		}

		void close(datapath::error why)
		{
			{
				std::lock_guard<std::mutex> l(lock);
				if (closed)
					return;
				closed = true;
				reason = why;
				closed_atomic.store(true, std::memory_order_release);
			}
			cv.notify_all();
		}

		// Lock held.
		void recycle(std::vector<uint8_t>&& buf)
		{
			if (spare.size() < 16 && buf.capacity() <= 64 * 1024) {
				buf.clear();
				spare.push_back(std::move(buf));
			}
		}
	};

	struct py_socket {
		std::shared_ptr<datapath::socket> sock;
		nb::object                        context; // keeps a custom Context alive
		nb::ft_mutex                      cb_lock;
		nb::object                        on_message_cb; // guarded by cb_lock (and the GIL)
		nb::object                        on_close_cb;
		std::atomic<bool>                 has_message_cb{false};
		std::atomic<bool>                 armed{false};
		std::chrono::microseconds         recv_spin{50};
		message_queue                     queue;
		std::atomic<size_t>               high_water{size_t(1) << 20};

		// Flow state; flow_lock orders pause()/resume() calls on the native socket.
		std::mutex        flow_lock;
		bool              user_paused = false; // pause()
		std::atomic<bool> native_paused{false};

		// Pause the native socket while the user paused or the recv() queue is at
		// the high-water mark, resume it otherwise. Never with queue.lock held.
		void update_flow()
		{
			std::lock_guard<std::mutex> l(flow_lock);
			bool want = user_paused || queue.bytes.load(std::memory_order_acquire) >= high_water.load(std::memory_order_relaxed);
			if (want == native_paused.load(std::memory_order_relaxed) || !sock)
				return;
			native_paused.store(want, std::memory_order_relaxed);
			if (want)
				sock->pause();
			else
				sock->resume();
		}

		// After taking from the queue: resume reading if it went below the mark.
		void after_take()
		{
			if (native_paused.load(std::memory_order_relaxed))
				update_flow();
		}

		void set_paused(bool on)
		{
			{
				std::lock_guard<std::mutex> l(flow_lock);
				user_paused = on;
			}
			update_flow();
		}

		py_socket(std::shared_ptr<datapath::socket> s, nb::handle ctx) : sock(std::move(s)), context(nb::borrow(ctx))
		{
			// Always track closure, so recv() wakes up and close_reason is known.
			sock->on_close([this](datapath::error reason) {
				handler_scope hs;
				queue.close(reason);
				python_entry entry;
				if (!entry)
					return;
				nb::gil_scoped_acquire gil;
				nb::object             cb;
				{
					nb::ft_lock_guard l(cb_lock);
					cb = on_close_cb;
				}
				if (!cb.is_valid() || cb.is_none())
					return;
				try {
					cb(nb::cast(reason));
				} catch (nb::python_error& e) {
					e.discard_as_unraisable("datapath: on_close callback");
				}
				// `this` may be gone now.
			});
		}

		~py_socket()
		{
			// Waits for a running handler, which may be waiting for the GIL.
			auto s = std::move(sock);
			{
				nb::gil_scoped_release release;
				s.reset();
			}
		}

		datapath::socket& get()
		{
			if (!sock)
				raise_error(datapath::error::Closed, "socket is closed");
			return *sock;
		}

		void arm()
		{
			if (armed.exchange(true))
				return;
			get().on_message([this](const uint8_t* data, size_t size) { deliver(data, size); });
		}

		void deliver(const uint8_t* data, size_t size)
		{
			handler_scope hs;
			if (!has_message_cb.load(std::memory_order_acquire)) {
				// Re-check under the queue lock: on_message() flips the flag under it
				// once the queue is drained, which keeps messages in order.
				bool queued = false, notify = false;
				{
					std::lock_guard<std::mutex> l(queue.lock);
					if (!has_message_cb.load(std::memory_order_acquire)) {
						queued = true;
						notify = queue.push_locked(data, size);
					}
				}
				if (notify)
					queue.wake();
				if (queued) {
					if (queue.bytes.load(std::memory_order_acquire) >= high_water.load(std::memory_order_relaxed))
						update_flow(); // stop reading until recv() caught up
					return;
				}
			}
			{
				python_entry entry;
				if (!entry)
					return;
				nb::gil_scoped_acquire gil;
				nb::object             cb;
				{
					nb::ft_lock_guard l(cb_lock);
					cb = on_message_cb;
				}
				if (cb.is_valid() && !cb.is_none()) {
					try {
						cb(nb::bytes(data, size));
					} catch (nb::python_error& e) {
						e.discard_as_unraisable("datapath: on_message callback");
					}
					return; // `this` may be gone now.
				}
			}
			// Callback cleared meanwhile: queue it.
			bool notify;
			{
				std::lock_guard<std::mutex> l(queue.lock);
				notify = queue.push_locked(data, size);
			}
			if (notify)
				queue.wake();
			if (queue.bytes.load(std::memory_order_acquire) >= high_water.load(std::memory_order_relaxed))
				update_flow();
		}

		void set_on_message(nb::object cb)
		{
			bool set = cb.is_valid() && !cb.is_none();
			if (set && !PyCallable_Check(cb.ptr()))
				throw nb::type_error("on_message expects a callable or None");
			if (!set) {
				has_message_cb.store(false, std::memory_order_release);
				nb::ft_lock_guard l(cb_lock);
				on_message_cb = nb::object();
				return;
			}
			{
				nb::ft_lock_guard l(cb_lock);
				on_message_cb = cb;
			}
			// Hand anything already queued to the callback, in order, then switch
			// delivery over under the queue lock so nothing overtakes it.
			for (;;) {
				std::vector<uint8_t> msg;
				{
					std::lock_guard<std::mutex> l(queue.lock);
					if (queue.items.empty()) {
						has_message_cb.store(true, std::memory_order_release);
						break;
					}
					msg = queue.pop_locked();
				}
				cb(nb::bytes(msg.data(), msg.size()));
			}
			after_take();
			arm();
		}

		void set_on_close(nb::object cb)
		{
			bool set = cb.is_valid() && !cb.is_none();
			if (set && !PyCallable_Check(cb.ptr()))
				throw nb::type_error("on_close expects a callable or None");
			{
				nb::ft_lock_guard l(cb_lock);
				on_close_cb = set ? std::move(cb) : nb::object();
			}
			// A peer's disconnect is only seen while reading, so start reading
			// (into the recv() queue unless on_message is set).
			if (set)
				arm();
		}

		void send(nb::handle data)
		{
			auto&     s = get();
			Py_buffer view;
			if (PyObject_GetBuffer(data.ptr(), &view, PyBUF_SIMPLE) != 0)
				throw nb::python_error();
			datapath::error ec;
			if (view.len >= 256 * 1024) {
				// Large copies: let other Python threads run meanwhile.
				nb::gil_scoped_release release;
				ec = s.send(view.buf, size_t(view.len));
			} else {
				ec = s.send(view.buf, size_t(view.len));
			}
			PyBuffer_Release(&view);
			check(ec, "send failed");
		}

		void sendv(nb::iterable parts)
		{
			auto&                              s = get();
			std::vector<Py_buffer>             views;
			std::vector<datapath::buffer_view> bv;
			size_t                             total       = 0;
			auto                               release_all = [&] {
                for (auto& v : views)
                    PyBuffer_Release(&v);
			};
			try {
				for (nb::handle part : parts) {
					Py_buffer v;
					if (PyObject_GetBuffer(part.ptr(), &v, PyBUF_SIMPLE) != 0)
						throw nb::python_error();
					views.push_back(v);
					bv.push_back({v.buf, size_t(v.len)});
					total += size_t(v.len);
				}
			} catch (...) {
				release_all();
				throw;
			}
			datapath::error ec;
			if (total >= 256 * 1024) {
				nb::gil_scoped_release release;
				ec = s.sendv(bv.data(), bv.size());
			} else {
				ec = s.sendv(bv.data(), bv.size());
			}
			release_all();
			check(ec, "send failed");
		}

		void reject_if_callback(const char* what)
		{
			if (has_message_cb.load(std::memory_order_acquire))
				throw std::runtime_error(std::string(what) + " can't be used while an on_message callback is set");
		}

		nb::bytes recv(std::optional<double> timeout)
		{
			warn_deprecated("Socket.recv()");
			reject_if_callback("recv()");
			arm();
			auto                 deadline = deadline_from(timeout);
			std::vector<uint8_t> msg;
			auto                 take = [&] {
                if (queue.items.empty())
                    return false;
                msg = queue.pop_locked();
                return true;
			};
			// Fast path: a message is already waiting.
			bool got;
			{
				std::lock_guard<std::mutex> l(queue.lock);
				got = take();
			}
			if (!got && !wait_queue(queue, deadline, recv_spin, take))
				raise_error(queue.reason, "connection closed");
			after_take();
			nb::bytes out(msg.data(), msg.size());
			{
				std::lock_guard<std::mutex> l(queue.lock);
				queue.recycle(std::move(msg));
			}
			return out;
		}

		size_t recv_into(nb::handle buffer, std::optional<double> timeout)
		{
			warn_deprecated("Socket.recv_into()");
			reject_if_callback("recv_into()");
			Py_buffer view;
			if (PyObject_GetBuffer(buffer.ptr(), &view, PyBUF_SIMPLE | PyBUF_WRITABLE) != 0)
				throw nb::python_error();
			struct guard {
				Py_buffer* v;
				~guard()
				{
					PyBuffer_Release(v);
				}
			} g{&view};

			arm();
			auto   deadline = deadline_from(timeout);
			size_t n        = 0;
			bool   small    = false;
			auto   take     = [&] {
                if (queue.items.empty())
                    return false;
                auto& front = queue.items.front();
                if (front.size() > size_t(view.len)) {
                    small = true; // leave it queued
                    n     = front.size();
                    return true;
                }
                n = front.size();
                std::memcpy(view.buf, front.data(), n);
                auto msg = queue.pop_locked();
                queue.recycle(std::move(msg));
                return true;
			};
			bool got;
			{
				std::lock_guard<std::mutex> l(queue.lock);
				got = take();
			}
			if (!got && !wait_queue(queue, deadline, recv_spin, take))
				raise_error(queue.reason, "connection closed");
			after_take();
			if (small)
				throw nb::value_error(
					("buffer too small: next message is " + std::to_string(n) + " bytes; it stays queued").c_str());
			return n;
		}

		size_t pending()
		{
			return queue.count.load(std::memory_order_acquire);
		}

		void close()
		{
			if (sock)
				sock->close();
		}

		bool good()
		{
			return sock && sock->good();
		}

		nb::object close_reason()
		{
			std::lock_guard<std::mutex> l(queue.lock);
			if (!queue.closed)
				return nb::none();
			return nb::cast(queue.reason);
		}
	};

	nb::object wrap_socket(std::shared_ptr<datapath::socket> s, nb::handle context)
	{
		return nb::cast(new py_socket(std::move(s), context), nb::rv_policy::take_ownership);
	}

	int socket_traverse(PyObject* self, visitproc visit, void* arg)
	{
		Py_VISIT(Py_TYPE(self));
		if (!nb::inst_ready(self))
			return 0;
		auto* s = nb::inst_ptr<py_socket>(self);
		Py_VISIT(s->on_message_cb.ptr());
		Py_VISIT(s->on_close_cb.ptr());
		Py_VISIT(s->context.ptr());
		return 0;
	}

	int socket_clear(PyObject* self)
	{
		auto*      s = nb::inst_ptr<py_socket>(self);
		nb::object a, b, c;
		{
			nb::ft_lock_guard l(s->cb_lock);
			s->has_message_cb.store(false, std::memory_order_release);
			a = std::move(s->on_message_cb);
			b = std::move(s->on_close_cb);
		}
		c = std::move(s->context);
		return 0;
	}

	PyType_Slot socket_slots[] = {
		{Py_tp_traverse, (void*)socket_traverse}, {Py_tp_clear, (void*)socket_clear}, {0, nullptr}};

	// ---------------------------------------------------------------------------
	// Server

	struct accept_queue {
		std::mutex                                    lock;
		std::condition_variable                       cv;
		std::deque<std::shared_ptr<datapath::socket>> items;
		std::atomic<size_t>                           count{0};
		std::atomic<bool>                             closed_atomic{false};
		int                                           waiters = 0;
		bool                                          closed  = false;

		bool closed_flag() const noexcept
		{
			return closed_atomic.load(std::memory_order_acquire);
		}

		void push(std::shared_ptr<datapath::socket> s)
		{
			bool wake;
			{
				std::lock_guard<std::mutex> l(lock);
				items.push_back(std::move(s));
				count.fetch_add(1, std::memory_order_release);
				wake = waiters > 0;
			}
			if (wake)
				cv.notify_one();
		}

		void close()
		{
			{
				std::lock_guard<std::mutex> l(lock);
				closed = true;
				closed_atomic.store(true, std::memory_order_release);
			}
			cv.notify_all();
		}
	};

	struct py_server {
		std::shared_ptr<datapath::server> srv;
		nb::object                        context;
		nb::ft_mutex                      cb_lock;
		nb::object                        on_accept_cb;
		std::atomic<bool>                 has_accept_cb{false};
		std::atomic<bool>                 armed{false};
		std::chrono::microseconds         accept_spin{0};
		accept_queue                      queue;

		py_server(std::shared_ptr<datapath::server> s, nb::handle ctx) : srv(std::move(s)), context(nb::borrow(ctx)) {}

		~py_server()
		{
			auto                                          s = std::move(srv);
			std::deque<std::shared_ptr<datapath::socket>> pending;
			{
				std::lock_guard<std::mutex> l(queue.lock);
				pending.swap(queue.items);
			}
			nb::gil_scoped_release release;
			s.reset();
			pending.clear();
		}

		datapath::server& get()
		{
			if (!srv)
				raise_error(datapath::error::Closed, "server is closed");
			return *srv;
		}

		void arm()
		{
			if (armed.exchange(true))
				return;
			get().on_accept([this](std::shared_ptr<datapath::socket> client) { return offer(std::move(client)); });
		}

		bool offer(std::shared_ptr<datapath::socket> client)
		{
			handler_scope hs;
			if (has_accept_cb.load(std::memory_order_acquire)) {
				python_entry entry;
				if (!entry)
					return false;
				nb::gil_scoped_acquire gil;
				nb::object             cb;
				{
					nb::ft_lock_guard l(cb_lock);
					cb = on_accept_cb;
				}
				if (cb.is_valid() && !cb.is_none()) {
					nb::object ctx = context;
					try {
						nb::object sock   = wrap_socket(std::move(client), ctx);
						nb::object result = cb(sock);
						// Only an explicit False rejects; None (no return) accepts.
						return !(result.is(nb::handle(Py_False)));
					} catch (nb::python_error& e) {
						e.discard_as_unraisable("datapath: on_accept callback");
						return false;
					}
				}
			}
			queue.push(std::move(client));
			return true;
		}

		void set_on_accept(nb::object cb)
		{
			bool set = cb.is_valid() && !cb.is_none();
			if (set && !PyCallable_Check(cb.ptr()))
				throw nb::type_error("on_accept expects a callable or None");
			{
				nb::ft_lock_guard l(cb_lock);
				on_accept_cb = set ? std::move(cb) : nb::object();
			}
			has_accept_cb.store(set, std::memory_order_release);
			if (set) {
				// Offer clients already queued by accept() mode.
				for (;;) {
					std::shared_ptr<datapath::socket> s;
					{
						std::lock_guard<std::mutex> l(queue.lock);
						if (queue.items.empty())
							break;
						s = std::move(queue.items.front());
						queue.items.pop_front();
						queue.count.fetch_sub(1, std::memory_order_relaxed);
					}
					nb::object cbo;
					{
						nb::ft_lock_guard l(cb_lock);
						cbo = on_accept_cb;
					}
					nb::object sock   = wrap_socket(std::move(s), context);
					nb::object result = cbo(sock);
					if (result.is(nb::handle(Py_False)))
						nb::cast<py_socket&>(sock).close();
				}
				arm();
			}
		}

		nb::object accept(std::optional<double> timeout)
		{
			warn_deprecated("Server.accept()");
			if (has_accept_cb.load(std::memory_order_acquire))
				throw std::runtime_error("accept() can't be used while an on_accept callback is set");
			arm();
			auto                              deadline = deadline_from(timeout);
			std::shared_ptr<datapath::socket> s;
			auto                              take = [&] {
                if (queue.items.empty())
                    return false;
                s = std::move(queue.items.front());
                queue.items.pop_front();
                queue.count.fetch_sub(1, std::memory_order_relaxed);
                return true;
			};
			bool got;
			{
				std::lock_guard<std::mutex> l(queue.lock);
				got = take();
			}
			if (!got && !wait_queue(queue, deadline, accept_spin, take))
				raise_error(datapath::error::Closed, "server closed");
			return wrap_socket(std::move(s), context);
		}

		void close()
		{
			if (srv)
				srv->close();
			queue.close();
		}
	};

	int server_traverse(PyObject* self, visitproc visit, void* arg)
	{
		Py_VISIT(Py_TYPE(self));
		if (!nb::inst_ready(self))
			return 0;
		auto* s = nb::inst_ptr<py_server>(self);
		Py_VISIT(s->on_accept_cb.ptr());
		Py_VISIT(s->context.ptr());
		return 0;
	}

	int server_clear(PyObject* self)
	{
		auto*      s = nb::inst_ptr<py_server>(self);
		nb::object a, c;
		{
			nb::ft_lock_guard l(s->cb_lock);
			s->has_accept_cb.store(false, std::memory_order_release);
			a = std::move(s->on_accept_cb);
		}
		c = std::move(s->context);
		return 0;
	}

	PyType_Slot server_slots[] = {
		{Py_tp_traverse, (void*)server_traverse}, {Py_tp_clear, (void*)server_clear}, {0, nullptr}};

	// ---------------------------------------------------------------------------
	// Options

	datapath::options make_options(std::optional<uint64_t> max_message_size, std::optional<uint64_t> max_queued_bytes,
								   std::optional<size_t> receive_buffer)
	{
		datapath::options o;
		if (max_message_size)
			o.max_message_size = *max_message_size;
		if (max_queued_bytes)
			o.max_queued_bytes = *max_queued_bytes;
		if (receive_buffer)
			o.receive_buffer = *receive_buffer;
		return o;
	}

	std::chrono::microseconds spin_from(int64_t us)
	{
		if (us < 0)
			throw nb::value_error("spin_us must be non-negative");
		return std::chrono::microseconds(us);
	}
} // namespace

NB_MODULE(_core, m)
{
	m.doc() = "DataPath native module. Use the `datapath` package instead of importing this directly.";

	nb::module_ errors = nb::module_::import_("datapath._errors");
	g_errors           = errors.ptr();
	Py_INCREF(g_errors); // lives for the whole process

	nb::enum_<datapath::error>(m, "Error", nb::is_arithmetic(), "Error codes reported by DataPath.")
		.value("Unknown", datapath::error::Unknown)
		.value("Success", datapath::error::Success)
		.value("Failure", datapath::error::Failure)
		.value("CriticalFailure", datapath::error::CriticalFailure)
		.value("Closed", datapath::error::Closed)
		.value("TimedOut", datapath::error::TimedOut)
		.value("InvalidPath", datapath::error::InvalidPath)
		.value("NotSupported", datapath::error::NotSupported)
		.value("TooLarge", datapath::error::TooLarge)
		.value("WouldBlock", datapath::error::WouldBlock)
		.value("VersionMismatch", datapath::error::VersionMismatch)
		.value("NotFound", datapath::error::NotFound)
		.value("AddressInUse", datapath::error::AddressInUse)
		.value("AccessDenied", datapath::error::AccessDenied);

	nb::enum_<datapath::permissions>(m, "Permissions", "Who may connect to a hosted server.")
		.value("User", datapath::permissions::User, "Only the user that created the server.")
		.value("Group", datapath::permissions::Group,
			   "The creating user and the local user group (Windows: BUILTIN\\Users, Linux: primary group).")
		.value("World", datapath::permissions::World, "Everyone on the local machine.");

	nb::class_<py_context>(m, "Context",
						   "Owns I/O threads. Sockets and servers created with it run on its threads.\n\n"
						   "Closing (or dropping) a context closes every socket and server on it.")
		.def(
			"__init__",
			[](py_context* self, int64_t spin_us, size_t io_threads, int priority, bool mmcss, int ideal_core,
			   bool alertable, bool yield_, bool pair_peer) {
				datapath::context_options o;
				o.spin       = spin_from(spin_us);
				o.io_threads = io_threads;
				o.priority   = priority;
				o.mmcss      = mmcss;
				o.ideal_core = ideal_core;
				o.alertable  = alertable;
				o.yield      = yield_;
				o.pair_peer  = pair_peer;
				std::shared_ptr<datapath::context> c;
				{
					nb::gil_scoped_release release;
					c = datapath::context::create(o);
				}
				if (!c)
					raise_error(datapath::error::CriticalFailure, "could not create context");
				new (self) py_context(std::move(c));
			},
			nb::kw_only(), "spin_us"_a = 50, "io_threads"_a = 1, "priority"_a = 2, "mmcss"_a = false,
			"ideal_core"_a = -1, "alertable"_a = false, "yield_"_a = false, "pair_peer"_a = false)
		.def("close", &py_context::close, "Close every socket and server on this context and join its threads.")
		.def_prop_ro(
			"closed", [](py_context& c) { return !c.ctx; }, "True once close() was called.")
		.def(
			"__enter__", [](nb::handle self) { return nb::borrow(self); }, nb::sig("def __enter__(self) -> Context"))
		.def("__exit__", [](py_context& c, nb::args) { c.close(); }, nb::sig("def __exit__(self, *args) -> None"));

	nb::class_<py_socket>(m, "Socket", nb::type_slots(socket_slots),
						  "One end of a connection. Dropping the last reference closes it.")
		.def("send", &py_socket::send, "data"_a, nb::sig("def send(self, data: collections.abc.Buffer, /) -> None"),
			 "Queue one message. Never blocks. Accepts any contiguous bytes-like object.\n\n"
			 "Raises TooLargeError, WouldBlockError or ClosedError when the message is not queued.")
		.def("sendv", &py_socket::sendv, "parts"_a,
			 nb::sig("def sendv(self, parts: collections.abc.Iterable[collections.abc.Buffer], /) -> None"),
			 "Queue one message assembled from several bytes-like parts.")
		.def("recv", &py_socket::recv, "timeout"_a = nb::none(),
			 "Deprecated since 1.1, removed in 2.0: use on_message().\n\n"
			 "Return the next message. Waits up to `timeout` seconds (None = forever).\n\n"
			 "Raises TimedOutError on timeout and ClosedError once the connection is closed\n"
			 "and every received message was returned.")
		.def("recv_into", &py_socket::recv_into, "buffer"_a, "timeout"_a = nb::none(),
			 nb::sig("def recv_into(self, buffer: collections.abc.Buffer, /, timeout: float | None = None) -> int"),
			 "Deprecated since 1.1, removed in 2.0: use on_message().\n\n"
			 "Copy the next message into a writable buffer and return its size.\n\n"
			 "Raises ValueError (and keeps the message queued) if the buffer is too small.")
		.def("on_message", &py_socket::set_on_message, "callback"_a.none(),
			 nb::sig("def on_message(self, callback: collections.abc.Callable[[bytes], object] | None, /) -> None"),
			 "Call `callback(data: bytes)` on an I/O thread for every message, instead of\n"
			 "queueing them for recv(). Messages already queued are handed over first.\n"
			 "None switches back to recv().")
		.def("on_close", &py_socket::set_on_close, "callback"_a.none(),
			 nb::sig("def on_close(self, callback: collections.abc.Callable[[Error], object] | None, /) -> None"),
			 "Call `callback(reason: Error)` on an I/O thread once, when the connection ends.\n"
			 "This starts receiving: messages queue up for recv() until on_message is set.")
		.def("close", &py_socket::close, "Close the connection. Unsent messages are dropped.")
		.def_prop_ro("good", &py_socket::good, "True while the connection is open.")
		.def_prop_ro(
			"closed", [](py_socket& s) { return !s.good(); }, "True once the connection has closed.")
		.def_prop_ro("close_reason", &py_socket::close_reason, nb::sig("def close_reason(self) -> Error | None"),
					 "Why the connection ended, or None while it is open.")
		.def_prop_ro(
			"max_message_size", [](py_socket& s) { return s.get().max_message_size(); },
			"Negotiated message size limit of this connection.")
		.def_prop_ro("pending", &py_socket::pending, "Messages received and waiting for recv().")
		.def_prop_ro(
			"pending_bytes", [](py_socket& s) { return s.queue.bytes.load(std::memory_order_acquire); },
			"Bytes waiting for recv(): each message counts its size plus 64.")
		.def_prop_rw(
			"recv_high_water_mark", [](py_socket& s) { return s.high_water.load(std::memory_order_relaxed); },
			[](py_socket& s, size_t v) {
				if (v < 1)
					throw nb::value_error("recv_high_water_mark must be at least 1");
				s.high_water.store(v, std::memory_order_relaxed);
				s.update_flow();
			},
			"Bytes that may wait for recv() before the socket stops reading from the peer\n"
			"(each message counts its size plus 64). The peer's sends then back up and\n"
			"fail with WouldBlockError. Default 1 MiB.")
		.def(
			"pause", [](py_socket& s) { s.set_paused(true); },
			"Stop reading from the peer; its sends back up. Messages already received\n"
			"can still be taken with recv(), and callbacks stop after the current one.")
		.def(
			"resume", [](py_socket& s) { s.set_paused(false); }, "Read from the peer again after pause().")
		.def_prop_rw(
			"recv_spin_us", [](py_socket& s) { return int64_t(s.recv_spin.count()); },
			[](py_socket& s, int64_t us) { s.recv_spin = spin_from(us); },
			"How long recv() busy-waits, with the GIL released, before it sleeps. Default 50.")
		.def(
			"__enter__", [](nb::handle self) { return nb::borrow(self); }, nb::sig("def __enter__(self) -> Socket"))
		.def("__exit__", [](py_socket& s, nb::args) { s.close(); }, nb::sig("def __exit__(self, *args) -> None"));

	nb::class_<py_server>(m, "Server", nb::type_slots(server_slots),
						  "A hosted endpoint. Dropping the last reference closes it.")
		.def("accept", &py_server::accept, "timeout"_a = nb::none(),
			 nb::sig("def accept(self, timeout: float | None = None) -> Socket"),
			 "Deprecated since 1.1, removed in 2.0: use on_accept().\n\n"
			 "Return the next client that completed the handshake. Waits up to `timeout`\n"
			 "seconds (None = forever). Raises TimedOutError or ClosedError.")
		.def("on_accept", &py_server::set_on_accept, "callback"_a.none(),
			 nb::sig("def on_accept(self, callback: collections.abc.Callable[[Socket], object] | None, /) -> None"),
			 "Call `callback(client: Socket)` on an I/O thread for each client, instead of\n"
			 "queueing them for accept(). Returning False rejects the client; anything else\n"
			 "accepts it. Keep a reference to the socket, or the connection closes.\n"
			 "Clients that are already waiting for accept() are offered first; for those,\n"
			 "False closes the connection. Pass on_accept= to host() to avoid that case.")
		.def("close", &py_server::close, "Stop accepting clients. Connected clients stay open.")
		.def_prop_rw(
			"accept_spin_us", [](py_server& s) { return int64_t(s.accept_spin.count()); },
			[](py_server& s, int64_t us) { s.accept_spin = spin_from(us); },
			"How long accept() busy-waits before it sleeps. Default 0.")
		.def(
			"__enter__", [](nb::handle self) { return nb::borrow(self); }, nb::sig("def __enter__(self) -> Server"))
		.def("__exit__", [](py_server& s, nb::args) { s.close(); }, nb::sig("def __exit__(self, *args) -> None"));

	m.def(
		"connect",
		[](const std::string& path, double connect_timeout, std::optional<uint64_t> max_message_size,
		   std::optional<uint64_t> max_queued_bytes, std::optional<size_t> receive_buffer, nb::object context) {
			auto o = make_options(max_message_size, max_queued_bytes, receive_buffer);
			if (connect_timeout < 0)
				throw nb::value_error("connect_timeout must be non-negative");
			o.connect_timeout                     = std::chrono::milliseconds(int64_t(connect_timeout * 1000.0 + 0.5));
			auto                              ctx = context_of(context);
			std::shared_ptr<datapath::socket> s;
			datapath::error                   ec;
			{
				nb::gil_scoped_release release;
				ec = datapath::connect(s, path, o, ctx);
			}
			check(ec, "connect failed");
			return wrap_socket(std::move(s), context);
		},
		"path"_a, nb::kw_only(), "connect_timeout"_a = 1.0, "max_message_size"_a = nb::none(),
		"max_queued_bytes"_a = nb::none(), "receive_buffer"_a = nb::none(), "context"_a = nb::none(),
		nb::sig("def connect(path: str, *, connect_timeout: float = 1.0, max_message_size: int | None = None, "
				"max_queued_bytes: int | None = None, receive_buffer: int | None = None, "
				"context: Context | None = None) -> Socket"),
		"Connect to a server. Blocks until the handshake completes or `connect_timeout`\n"
		"seconds pass. Without `context`, the process-wide default context is used.");

	m.def(
		"host",
		[](const std::string& path, datapath::permissions permissions, size_t backlog, size_t max_clients,
		   double handshake_timeout, std::optional<uint64_t> max_message_size,
		   std::optional<uint64_t> max_queued_bytes, std::optional<size_t> receive_buffer,
		   std::optional<size_t> pipe_buffer, nb::object context, nb::object on_accept) {
			if (!(handshake_timeout >= 0))
				throw nb::value_error("handshake_timeout must be non-negative");
			auto o        = make_options(max_message_size, max_queued_bytes, receive_buffer);
			o.permissions = permissions;
			o.backlog     = backlog;
			o.max_clients = max_clients;
			o.handshake_timeout = std::chrono::milliseconds(int64_t(handshake_timeout * 1000.0 + 0.5));
			if (pipe_buffer)
				o.pipe_buffer = *pipe_buffer;
			auto                              ctx = context_of(context);
			std::shared_ptr<datapath::server> s;
			datapath::error                   ec;
			{
				nb::gil_scoped_release release;
				ec = datapath::host(s, path, o, ctx);
			}
			check(ec, "host failed");
			auto*      ps  = new py_server(std::move(s), context);
			nb::object out = nb::cast(ps, nb::rv_policy::take_ownership);
			// Accept right away (into the accept() queue, or the callback below):
			// a client's connect() only completes once the server accepted it.
			if (on_accept.is_valid() && !on_accept.is_none())
				ps->set_on_accept(on_accept);
			else
				ps->arm();
			return out;
		},
		"path"_a, nb::kw_only(), "permissions"_a = datapath::permissions::User, "backlog"_a = 8, "max_clients"_a = 0,
		"handshake_timeout"_a = 5.0, "max_message_size"_a = nb::none(), "max_queued_bytes"_a = nb::none(), "receive_buffer"_a = nb::none(),
		"pipe_buffer"_a = nb::none(), "context"_a = nb::none(), "on_accept"_a = nb::none(),
		nb::sig("def host(path: str, *, permissions: Permissions = ..., backlog: int = 8, "
				"max_clients: int = 0, handshake_timeout: float = 5.0, max_message_size: int | None = None, "
				"max_queued_bytes: int | None = None, receive_buffer: int | None = None, "
				"pipe_buffer: int | None = None, context: Context | None = None, "
				"on_accept: collections.abc.Callable[[Socket], object] | None = None) -> Server"),
		"Host a server at `path`. A bare name maps to \\\\.\\pipe\\<name> on Windows and to\n"
		"the abstract socket datapath.<name> on Linux; on Linux a path starting with / is\n"
		"a filesystem socket.\n\n"
		"Clients are accepted at once and wait for accept(), unless `on_accept` is given\n"
		"(see Server.on_accept()). A client that does not send its handshake within\n"
		"`handshake_timeout` seconds is disconnected (0 = no limit).");

	m.def("_shutdown", &shutdown, "Internal: called at interpreter exit.");
}
