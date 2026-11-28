/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Node-API addon on top of the C++ API.
//
// Threading model: datapath calls its handlers on I/O threads, but JavaScript
// may only run on the thread that owns the napi_env. Every native socket and
// server therefore owns a "channel": a mutex-protected event queue plus one
// thread-safe function (TSFN). I/O threads push events and signal the TSFN only
// when the queue goes from empty to non-empty, so a burst of messages costs one
// wake-up of the event loop instead of one per message. The JS thread drains the
// whole queue per wake-up.
//
// Events pushed before JavaScript attached its dispatcher (an accepted socket
// whose JS object does not exist yet) wait in the queue until attach().

#include <cstring>
#include <datapath/datapath.hpp>
#include <memory>
#include <mutex>
#include <napi.h>
#include <string>
#include <utility>
#include <vector>

namespace {
	enum class event_kind : int32_t {
		Message = 0,
		Close   = 1,
		Accept  = 2,
	};

	class socket_state;

	// Messages up to this size are copied into a regular Buffer on the JS thread.
	constexpr size_t copy_threshold = size_t(64) * 1024;

	struct event {
		explicit event(event_kind k) : kind(k) {}

		event_kind                    kind;
		std::unique_ptr<uint8_t[]>    data;
		size_t                        size   = 0;
		datapath::error               reason = datapath::error::Success;
		std::shared_ptr<socket_state> accepted;
	};

	// Per-env data: constructors used to create wrapper objects from C++.
	struct addon_data {
		Napi::FunctionReference socket_ctor;
		Napi::FunctionReference server_ctor;
		Napi::FunctionReference context_ctor;
	};

	// Event queue + thread-safe function shared by sockets and servers.
	class channel : public std::enable_shared_from_this<channel> {
		std::mutex         _lock;
		std::vector<event> _events;
		bool               _signaled = false;
		// TSFN exists and may be called. Cleared on release and on finalize.
		bool _live = false;
		// No more events are accepted (released or torn down).
		bool                     _done = false;
		napi_threadsafe_function _tsfn = nullptr;

		static void call_js(napi_env env, napi_value js_cb, void* context, void* /*data*/)
		{
			auto self = *static_cast<std::shared_ptr<channel>*>(context);
			if (!env || !js_cb)
				return;
			self->drain(env, js_cb);
		}

		static void finalize(napi_env /*env*/, void* finalize_data, void* /*hint*/)
		{
			auto holder = static_cast<std::shared_ptr<channel>*>(finalize_data);
			auto self   = *holder;
			delete holder;
			{
				std::lock_guard<std::mutex> l(self->_lock);
				self->_live = false;
				self->_done = true;
				self->_tsfn = nullptr;
			}
			// Env teardown or normal release: make sure the native side stops too.
			self->on_finalize();
		}

		void drain(napi_env env, napi_value js_cb)
		{
			std::vector<event> batch;
			{
				std::lock_guard<std::mutex> l(_lock);
				batch.swap(_events);
				_signaled = false;
			}
			napi_value recv;
			napi_get_undefined(env, &recv);
			for (size_t i = 0; i < batch.size(); i++) {
				napi_handle_scope scope;
				napi_open_handle_scope(env, &scope);
				bool ok = dispatch(env, js_cb, recv, batch[i]);
				napi_close_handle_scope(env, scope);
				if (!ok) {
					// A listener threw. Keep the remaining events for the next
					// wake-up and let Node report the exception.
					std::lock_guard<std::mutex> l(_lock);
					_events.insert(_events.begin(), std::make_move_iterator(batch.begin() + i + 1),
								   std::make_move_iterator(batch.end()));
					if (!_events.empty() && _live) {
						_signaled = true;
						napi_call_threadsafe_function(_tsfn, nullptr, napi_tsfn_nonblocking);
					}
					return;
				}
			}
		}

		protected:
		// Convert one event and call the JS dispatcher. Returns false if it threw.
		virtual bool dispatch(napi_env env, napi_value js_cb, napi_value recv, event& ev) = 0;
		virtual void on_finalize() {}

		public:
		virtual ~channel() = default;

		// Any thread. Returns false when the event was dropped.
		bool push(event&& ev)
		{
			std::lock_guard<std::mutex> l(_lock);
			if (_done)
				return false;
			_events.push_back(std::move(ev));
			if (_live && !_signaled) {
				_signaled = true;
				napi_call_threadsafe_function(_tsfn, nullptr, napi_tsfn_nonblocking);
			}
			return true;
		}

		// JS thread. Creates the TSFN that calls `fn(kind, payload)`.
		void attach(Napi::Env env, Napi::Function fn, const char* name)
		{
			std::lock_guard<std::mutex> l(_lock);
			if (_tsfn || _done)
				NAPI_THROW_VOID(Napi::Error::New(env, "already attached"));
			napi_value resource_name;
			napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &resource_name);
			auto holder = new std::shared_ptr<channel>(shared_from_this());
			auto status = napi_create_threadsafe_function(env, fn, nullptr, resource_name, 0, 1, holder, finalize,
														  holder, call_js, &_tsfn);
			if (status != napi_ok) {
				delete holder;
				_tsfn = nullptr;
				NAPI_THROW_VOID(Napi::Error::New(env, "napi_create_threadsafe_function failed"));
			}
			_live = true;
			if (!_events.empty() && !_signaled) {
				_signaled = true;
				napi_call_threadsafe_function(_tsfn, nullptr, napi_tsfn_nonblocking);
			}
		}

		// JS thread. Stop accepting events and let the TSFN finalize. Events that
		// were already signaled are still delivered before it does.
		std::vector<event> release()
		{
			std::vector<event>          left;
			std::lock_guard<std::mutex> l(_lock);
			if (_done)
				return left;
			_done = true;
			left.swap(_events);
			if (_live) {
				_live = false;
				napi_release_threadsafe_function(_tsfn, napi_tsfn_release);
			}
			return left;
		}

		void ref(Napi::Env env, bool keep_alive)
		{
			std::lock_guard<std::mutex> l(_lock);
			if (!_live)
				return;
			if (keep_alive)
				napi_ref_threadsafe_function(env, _tsfn);
			else
				napi_unref_threadsafe_function(env, _tsfn);
		}
	};

	// Socket ---------------------------------------------------------------------

	class socket_state : public channel {
		public:
		std::shared_ptr<datapath::socket> sock;
		bool                              receiving = false;

		explicit socket_state(std::shared_ptr<datapath::socket> s) : sock(std::move(s)) {}

		~socket_state() override
		{
			// Waits for running handlers, then drops them.
			sock.reset();
		}

		// Install the close handler right away: datapath ignores a close handler
		// set after the connection already closed.
		static std::shared_ptr<socket_state> make(std::shared_ptr<datapath::socket> s)
		{
			auto                        st   = std::make_shared<socket_state>(std::move(s));
			std::weak_ptr<socket_state> weak = st;
			st->sock->on_close([weak](datapath::error reason) {
				if (auto self = weak.lock()) {
					event ev{event_kind::Close};
					ev.reason = reason;
					self->push(std::move(ev));
				}
			});
			return st;
		}

		void start_receiving()
		{
			if (receiving)
				return;
			receiving                        = true;
			std::weak_ptr<socket_state> weak = std::static_pointer_cast<socket_state>(shared_from_this());
			sock->on_message([weak](const uint8_t* data, size_t size) {
				auto self = weak.lock();
				if (!self)
					return;
				event ev{event_kind::Message};
				if (size) {
					ev.data.reset(new uint8_t[size]);
					std::memcpy(ev.data.get(), data, size);
				}
				ev.size = size;
				self->push(std::move(ev));
			});
		}

		protected:
		bool dispatch(napi_env env, napi_value js_cb, napi_value recv, event& ev) override
		{
			napi_value args[2];
			napi_create_int32(env, int32_t(ev.kind), &args[0]);
			bool close = false;
			if (ev.kind == event_kind::Message) {
				size_t size = ev.size;
				try {
					if (size <= copy_threshold) {
						// Small: a copy into a V8-owned buffer is cheaper than an
						// external buffer, whose GC finalizer costs more than the memcpy.
						args[1] = Napi::Buffer<uint8_t>::Copy(env, ev.data.get(), size);
					} else {
						uint8_t* raw = ev.data.release();
						args[1] =
							Napi::Buffer<uint8_t>::NewOrCopy(env, raw, size, [](Napi::Env, uint8_t* p) { delete[] p; });
					}
				} catch (const Napi::Error&) {
					return false;
				}
			} else {
				napi_create_int32(env, int32_t(ev.reason), &args[1]);
				close = true;
			}
			napi_value result;
			auto       status = napi_call_function(env, recv, js_cb, 2, args, &result);
			if (close)
				release();
			return status == napi_ok;
		}

		void on_finalize() override
		{
			// Env teardown (worker exit, process.exit): stop the connection.
			if (sock)
				sock->close();
		}
	};

	// Server ---------------------------------------------------------------------

	class server_state : public channel {
		public:
		std::shared_ptr<datapath::server> srv;

		~server_state() override
		{
			srv.reset();
		}

		protected:
		bool dispatch(napi_env env, napi_value js_cb, napi_value recv, event& ev) override;

		void on_finalize() override
		{
			if (srv)
				srv->close();
		}
	};

	// Context --------------------------------------------------------------------

	class context_wrap : public Napi::ObjectWrap<context_wrap> {
		public:
		std::shared_ptr<datapath::context> ctx;

		static Napi::Function init(Napi::Env env)
		{
			return DefineClass(env, "NativeContext", {});
		}

		context_wrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<context_wrap>(info)
		{
			Napi::Env                 env = info.Env();
			datapath::context_options opts;
			if (info.Length() > 0 && info[0].IsObject()) {
				auto o = info[0].As<Napi::Object>();
				if (o.Has("spin") && o.Get("spin").IsNumber())
					opts.spin = std::chrono::microseconds(int64_t(o.Get("spin").As<Napi::Number>().DoubleValue()));
				if (o.Has("ioThreads") && o.Get("ioThreads").IsNumber())
					opts.io_threads = size_t(o.Get("ioThreads").As<Napi::Number>().Int64Value());
				if (o.Has("mmcss") && o.Get("mmcss").IsBoolean())
					opts.mmcss = o.Get("mmcss").As<Napi::Boolean>().Value();
				if (o.Has("priority") && o.Get("priority").IsNumber())
					opts.priority = o.Get("priority").As<Napi::Number>().Int32Value();
				if (o.Has("idealCore") && o.Get("idealCore").IsNumber())
					opts.ideal_core = o.Get("idealCore").As<Napi::Number>().Int32Value();
				if (o.Has("alertable") && o.Get("alertable").IsBoolean())
					opts.alertable = o.Get("alertable").As<Napi::Boolean>().Value();
				if (o.Has("yield") && o.Get("yield").IsBoolean())
					opts.yield = o.Get("yield").As<Napi::Boolean>().Value();
				if (o.Has("pairPeer") && o.Get("pairPeer").IsBoolean())
					opts.pair_peer = o.Get("pairPeer").As<Napi::Boolean>().Value();
			}
			ctx = datapath::context::create(opts);
			if (!ctx)
				NAPI_THROW_VOID(Napi::Error::New(env, "could not create datapath context"));
		}
	};

	// Socket wrapper ---------------------------------------------------------------

	class socket_wrap : public Napi::ObjectWrap<socket_wrap> {
		public:
		std::shared_ptr<socket_state> state;

		static Napi::Function init(Napi::Env env)
		{
			return DefineClass(env, "NativeSocket",
							   {
								   InstanceMethod<&socket_wrap::attach>("attach"),
								   InstanceMethod<&socket_wrap::start>("start"),
								   InstanceMethod<&socket_wrap::send>("send"),
								   InstanceMethod<&socket_wrap::sendv>("sendv"),
								   InstanceMethod<&socket_wrap::close>("close"),
								   InstanceMethod<&socket_wrap::good>("good"),
								   InstanceMethod<&socket_wrap::max_message_size>("maxMessageSize"),
								   InstanceMethod<&socket_wrap::ref>("ref"),
								   InstanceMethod<&socket_wrap::unref>("unref"),
							   });
		}

		static Napi::Object wrap(Napi::Env env, std::shared_ptr<socket_state> st)
		{
			auto data          = env.GetInstanceData<addon_data>();
			auto obj           = data->socket_ctor.New({});
			Unwrap(obj)->state = std::move(st);
			return obj;
		}

		socket_wrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<socket_wrap>(info) {}

		bool check(const Napi::CallbackInfo& info)
		{
			if (!state) {
				Napi::Error::New(info.Env(), "NativeSocket is not connected").ThrowAsJavaScriptException();
				return false;
			}
			return true;
		}

		void attach(const Napi::CallbackInfo& info)
		{
			if (!check(info))
				return;
			if (info.Length() < 1 || !info[0].IsFunction())
				NAPI_THROW_VOID(Napi::TypeError::New(info.Env(), "dispatcher must be a function"));
			state->attach(info.Env(), info[0].As<Napi::Function>(), "datapath.socket");
		}

		void start(const Napi::CallbackInfo& info)
		{
			if (check(info))
				state->start_receiving();
		}

		static bool view_of(Napi::Value v, datapath::buffer_view& out, std::vector<std::string>& keep)
		{
			// DataView first: napi_is_buffer() is true for every ArrayBufferView.
			if (v.IsDataView()) {
				auto dv  = v.As<Napi::DataView>();
				out.data = static_cast<const uint8_t*>(dv.ArrayBuffer().Data()) + dv.ByteOffset();
				out.size = dv.ByteLength();
				return true;
			}
			if (v.IsTypedArray()) {
				auto ta  = v.As<Napi::TypedArray>();
				auto ab  = ta.ArrayBuffer();
				out.data = static_cast<const uint8_t*>(ab.Data()) + ta.ByteOffset();
				out.size = ta.ByteLength();
				return true;
			}
			if (v.IsArrayBuffer()) {
				auto ab  = v.As<Napi::ArrayBuffer>();
				out.data = ab.Data();
				out.size = ab.ByteLength();
				return true;
			}
			if (v.IsString()) {
				keep.emplace_back(v.As<Napi::String>().Utf8Value());
				out.data = keep.back().data();
				out.size = keep.back().size();
				return true;
			}
			return false;
		}

		// Returns the datapath error code; 0 = queued.
		Napi::Value send(const Napi::CallbackInfo& info)
		{
			if (!check(info))
				return info.Env().Undefined();
			datapath::buffer_view    part{};
			std::vector<std::string> keep;
			keep.reserve(1);
			if (info.Length() < 1 || !view_of(info[0], part, keep)) {
				Napi::TypeError::New(info.Env(), "data must be a Buffer, TypedArray, DataView, ArrayBuffer or string")
					.ThrowAsJavaScriptException();
				return info.Env().Undefined();
			}
			return Napi::Number::New(info.Env(), int32_t(state->sock->send(part.data, part.size)));
		}

		Napi::Value sendv(const Napi::CallbackInfo& info)
		{
			if (!check(info))
				return info.Env().Undefined();
			if (info.Length() < 1 || !info[0].IsArray()) {
				Napi::TypeError::New(info.Env(), "parts must be an array").ThrowAsJavaScriptException();
				return info.Env().Undefined();
			}
			auto                               arr = info[0].As<Napi::Array>();
			uint32_t                           n   = arr.Length();
			std::vector<datapath::buffer_view> parts(n);
			std::vector<std::string>           keep;
			keep.reserve(n); // no reallocation: views point into these strings
			for (uint32_t i = 0; i < n; i++) {
				if (!view_of(arr.Get(i), parts[i], keep)) {
					Napi::TypeError::New(info.Env(),
										 "every part must be a Buffer, TypedArray, DataView, "
										 "ArrayBuffer or string")
						.ThrowAsJavaScriptException();
					return info.Env().Undefined();
				}
			}
			return Napi::Number::New(info.Env(), int32_t(state->sock->sendv(parts.data(), parts.size())));
		}

		Napi::Value close(const Napi::CallbackInfo& info)
		{
			if (!check(info))
				return info.Env().Undefined();
			return Napi::Number::New(info.Env(), int32_t(state->sock->close()));
		}

		Napi::Value good(const Napi::CallbackInfo& info)
		{
			return Napi::Boolean::New(info.Env(), state && state->sock->good());
		}

		Napi::Value max_message_size(const Napi::CallbackInfo& info)
		{
			if (!check(info))
				return info.Env().Undefined();
			return Napi::Number::New(info.Env(), double(state->sock->max_message_size()));
		}

		void ref(const Napi::CallbackInfo& info)
		{
			if (check(info))
				state->ref(info.Env(), true);
		}

		void unref(const Napi::CallbackInfo& info)
		{
			if (check(info))
				state->ref(info.Env(), false);
		}
	};

	bool server_state::dispatch(napi_env env, napi_value js_cb, napi_value recv, event& ev)
	{
		napi_value args[2];
		napi_create_int32(env, int32_t(ev.kind), &args[0]);
		try {
			args[1] = socket_wrap::wrap(Napi::Env(env), std::move(ev.accepted));
		} catch (const Napi::Error&) {
			return false;
		}
		napi_value result;
		return napi_call_function(env, recv, js_cb, 2, args, &result) == napi_ok;
	}

	// Server wrapper ---------------------------------------------------------------

	class server_wrap : public Napi::ObjectWrap<server_wrap> {
		public:
		std::shared_ptr<server_state> state;

		static Napi::Function init(Napi::Env env)
		{
			return DefineClass(env, "NativeServer",
							   {
								   InstanceMethod<&server_wrap::attach>("attach"),
								   InstanceMethod<&server_wrap::close>("close"),
								   InstanceMethod<&server_wrap::ref>("ref"),
								   InstanceMethod<&server_wrap::unref>("unref"),
							   });
		}

		server_wrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<server_wrap>(info) {}

		void attach(const Napi::CallbackInfo& info)
		{
			if (info.Length() < 1 || !info[0].IsFunction())
				NAPI_THROW_VOID(Napi::TypeError::New(info.Env(), "dispatcher must be a function"));
			state->attach(info.Env(), info[0].As<Napi::Function>(), "datapath.server");
		}

		Napi::Value close(const Napi::CallbackInfo& info)
		{
			auto ec = state->srv->close();
			// Clients accepted but not yet seen by JavaScript close with their state.
			state->release();
			return Napi::Number::New(info.Env(), int32_t(ec));
		}

		void ref(const Napi::CallbackInfo& info)
		{
			state->ref(info.Env(), true);
		}

		void unref(const Napi::CallbackInfo& info)
		{
			state->ref(info.Env(), false);
		}
	};

	// Free functions -------------------------------------------------------------

	uint64_t to_u64(Napi::Value v)
	{
		if (v.IsBigInt()) {
			bool lossless;
			return v.As<Napi::BigInt>().Uint64Value(&lossless);
		}
		double d = v.As<Napi::Number>().DoubleValue();
		return d <= 0 ? 0 : uint64_t(d);
	}

	bool has(const Napi::Object& o, const char* key)
	{
		if (!o.Has(key))
			return false;
		auto v = o.Get(key);
		return !v.IsUndefined() && !v.IsNull();
	}

	datapath::options parse_options(const Napi::Value& value)
	{
		datapath::options opts;
		// Every message is copied into a Buffer of its own before JavaScript sees
		// it, so the peer can't change the bytes afterwards: skip the library's
		// own copy out of the shared-memory ring.
		opts.shared_memory_zero_copy = true;
		if (!value.IsObject())
			return opts;
		auto o = value.As<Napi::Object>();
		if (has(o, "maxMessageSize"))
			opts.max_message_size = to_u64(o.Get("maxMessageSize"));
		if (has(o, "maxQueuedBytes"))
			opts.max_queued_bytes = to_u64(o.Get("maxQueuedBytes"));
		if (has(o, "permissions")) {
			auto p = o.Get("permissions").ToString().Utf8Value();
			if (p == "group")
				opts.permissions = datapath::permissions::Group;
			else if (p == "world")
				opts.permissions = datapath::permissions::World;
			else
				opts.permissions = datapath::permissions::User;
		}
		if (has(o, "backlog"))
			opts.backlog = size_t(to_u64(o.Get("backlog")));
		if (has(o, "maxClients"))
			opts.max_clients = size_t(to_u64(o.Get("maxClients")));
		if (has(o, "connectTimeout"))
			opts.connect_timeout = std::chrono::milliseconds(int64_t(to_u64(o.Get("connectTimeout"))));
		if (has(o, "handshakeTimeout"))
			opts.handshake_timeout = std::chrono::milliseconds(int64_t(to_u64(o.Get("handshakeTimeout"))));
		if (has(o, "receiveBuffer"))
			opts.receive_buffer = size_t(to_u64(o.Get("receiveBuffer")));
		if (has(o, "pipeBuffer"))
			opts.pipe_buffer = size_t(to_u64(o.Get("pipeBuffer")));
		if (has(o, "sharedMemory"))
			opts.shared_memory = o.Get("sharedMemory").ToBoolean().Value();
		if (has(o, "sharedMemorySize"))
			opts.shared_memory_size = size_t(to_u64(o.Get("sharedMemorySize")));
		return opts;
	}

	std::shared_ptr<datapath::context> parse_context(Napi::Env env, const Napi::Value& value)
	{
		if (!value.IsObject())
			return nullptr;
		auto obj  = value.As<Napi::Object>();
		auto data = env.GetInstanceData<addon_data>();
		if (!obj.InstanceOf(data->context_ctor.Value()))
			return nullptr;
		return context_wrap::Unwrap(obj)->ctx;
	}

	class connect_worker : public Napi::AsyncWorker {
		Napi::Promise::Deferred            _deferred;
		std::string                        _path;
		datapath::options                  _options;
		std::shared_ptr<datapath::context> _ctx;
		datapath::error                    _ec = datapath::error::Unknown;
		std::shared_ptr<socket_state>      _state;

		public:
		connect_worker(Napi::Env env, std::string path, datapath::options opts, std::shared_ptr<datapath::context> ctx)
			: Napi::AsyncWorker(env, "datapath.connect"), _deferred(Napi::Promise::Deferred::New(env)),
			  _path(std::move(path)), _options(opts), _ctx(std::move(ctx))
		{}

		Napi::Promise promise() const
		{
			return _deferred.Promise();
		}

		void Execute() override
		{
			std::shared_ptr<datapath::socket> sock;
			_ec = datapath::connect(sock, _path, _options, _ctx);
			if (_ec == datapath::error::Success)
				_state = socket_state::make(std::move(sock));
		}

		void OnOK() override
		{
			Napi::HandleScope scope(Env());
			if (_ec != datapath::error::Success) {
				_deferred.Reject(Napi::Number::New(Env(), int32_t(_ec)));
				return;
			}
			_deferred.Resolve(socket_wrap::wrap(Env(), std::move(_state)));
		}

		void OnError(const Napi::Error& e) override
		{
			_deferred.Reject(e.Value());
		}
	};

	// connect(path, options, context) -> Promise<NativeSocket>; rejects with an error code.
	Napi::Value js_connect(const Napi::CallbackInfo& info)
	{
		Napi::Env env = info.Env();
		if (info.Length() < 1 || !info[0].IsString())
			NAPI_THROW(Napi::TypeError::New(env, "path must be a string"), env.Undefined());
		auto worker  = new connect_worker(env, info[0].As<Napi::String>().Utf8Value(),
										  parse_options(info.Length() > 1 ? info[1] : env.Undefined()),
										  parse_context(env, info.Length() > 2 ? info[2] : env.Undefined()));
		auto promise = worker->promise();
		worker->Queue();
		return promise;
	}

	// host(path, options, context) -> NativeServer, or an error code.
	Napi::Value js_host(const Napi::CallbackInfo& info)
	{
		Napi::Env env = info.Env();
		if (info.Length() < 1 || !info[0].IsString())
			NAPI_THROW(Napi::TypeError::New(env, "path must be a string"), env.Undefined());
		std::shared_ptr<datapath::server> srv;
		auto                              ec = datapath::host(srv, info[0].As<Napi::String>().Utf8Value(),
															  parse_options(info.Length() > 1 ? info[1] : env.Undefined()),
															  parse_context(env, info.Length() > 2 ? info[2] : env.Undefined()));
		if (ec != datapath::error::Success)
			return Napi::Number::New(env, int32_t(ec));

		auto st                          = std::make_shared<server_state>();
		st->srv                          = srv;
		std::weak_ptr<server_state> weak = st;
		srv->on_accept([weak](std::shared_ptr<datapath::socket> client) {
			auto self = weak.lock();
			if (!self)
				return false;
			event ev{event_kind::Accept};
			ev.accepted = socket_state::make(std::move(client));
			return self->push(std::move(ev));
		});

		auto data                       = env.GetInstanceData<addon_data>();
		auto obj                        = data->server_ctor.New({});
		server_wrap::Unwrap(obj)->state = std::move(st);
		return obj;
	}

	Napi::Value js_error_name(const Napi::CallbackInfo& info)
	{
		auto code = datapath::error(info[0].As<Napi::Number>().Int32Value());
		return Napi::String::New(info.Env(), datapath::to_string(code));
	}

	Napi::Object init(Napi::Env env, Napi::Object exports)
	{
		auto data          = new addon_data();
		auto sock          = socket_wrap::init(env);
		auto srv           = server_wrap::init(env);
		auto ctx           = context_wrap::init(env);
		data->socket_ctor  = Napi::Persistent(sock);
		data->server_ctor  = Napi::Persistent(srv);
		data->context_ctor = Napi::Persistent(ctx);
		env.SetInstanceData(data);

		exports.Set("NativeSocket", sock);
		exports.Set("NativeServer", srv);
		exports.Set("NativeContext", ctx);
		exports.Set("connect", Napi::Function::New(env, js_connect, "connect"));
		exports.Set("host", Napi::Function::New(env, js_host, "host"));
		exports.Set("errorName", Napi::Function::New(env, js_error_name, "errorName"));
		return exports;
	}
} // namespace

NODE_API_MODULE(datapath, init)
