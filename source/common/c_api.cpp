/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// C API: thin wrappers over the C++ API. Every entry point is noexcept; the
// hot paths (send, message delivery) add one indirect call and no allocation.

#include <datapath/datapath.h>
#include <datapath/datapath.hpp>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#if __has_include(<datapath/version.hpp>)
#include <datapath/version.hpp>
#define DP_HAS_VERSION_INFO 1
#else
#include "version.hpp"
#endif

struct datapath_context {
	std::shared_ptr<datapath::context> ctx;
};

struct datapath_socket {
	std::shared_ptr<datapath::socket> sock;
};

struct datapath_server {
	std::shared_ptr<datapath::server> srv;
};

namespace {
	using datapath::error;

#define DP_SAME(c, cpp) static_assert(int32_t(c) == int32_t(error::cpp), "C error code out of sync: " #cpp)
	DP_SAME(DATAPATH_ERROR_UNKNOWN, Unknown);
	DP_SAME(DATAPATH_SUCCESS, Success);
	DP_SAME(DATAPATH_ERROR_FAILURE, Failure);
	DP_SAME(DATAPATH_ERROR_CRITICAL_FAILURE, CriticalFailure);
	DP_SAME(DATAPATH_ERROR_CLOSED, Closed);
	DP_SAME(DATAPATH_ERROR_TIMED_OUT, TimedOut);
	DP_SAME(DATAPATH_ERROR_INVALID_PATH, InvalidPath);
	DP_SAME(DATAPATH_ERROR_NOT_SUPPORTED, NotSupported);
	DP_SAME(DATAPATH_ERROR_TOO_LARGE, TooLarge);
	DP_SAME(DATAPATH_ERROR_WOULD_BLOCK, WouldBlock);
	DP_SAME(DATAPATH_ERROR_VERSION_MISMATCH, VersionMismatch);
	DP_SAME(DATAPATH_ERROR_NOT_FOUND, NotFound);
	DP_SAME(DATAPATH_ERROR_ADDRESS_IN_USE, AddressInUse);
	DP_SAME(DATAPATH_ERROR_ACCESS_DENIED, AccessDenied);
#undef DP_SAME

	static_assert(int(DATAPATH_PERMISSIONS_USER) == int(datapath::permissions::User), "permissions out of sync");
	static_assert(int(DATAPATH_PERMISSIONS_GROUP) == int(datapath::permissions::Group), "permissions out of sync");
	static_assert(int(DATAPATH_PERMISSIONS_WORLD) == int(datapath::permissions::World), "permissions out of sync");

	// sendv passes the caller's array straight through.
	static_assert(sizeof(datapath_buffer) == sizeof(datapath::buffer_view), "buffer layout");
	static_assert(offsetof(datapath_buffer, data) == offsetof(datapath::buffer_view, data), "buffer layout");
	static_assert(offsetof(datapath_buffer, size) == offsetof(datapath::buffer_view, size), "buffer layout");

	inline datapath_error to_c(error e) noexcept
	{
		return datapath_error(e);
	}

	// Runs `fn`, turning any exception into an error code.
	template<typename Fn>
	datapath_error guarded(Fn&& fn) noexcept
	{
		try {
			return fn();
		} catch (const std::bad_alloc&) {
			return DATAPATH_ERROR_OUT_OF_MEMORY;
		} catch (...) {
			return DATAPATH_ERROR_CRITICAL_FAILURE;
		}
	}

	// Owns a callback's user data; frees it when the last copy of the handler goes.
	struct user_data {
		void*            user;
		datapath_free_fn free;
		user_data(void* u, datapath_free_fn f) noexcept : user(u), free(f) {}
		user_data(const user_data&)            = delete;
		user_data& operator=(const user_data&) = delete;
		~user_data()
		{
			free(user);
		}
	};

	std::shared_ptr<user_data> own(void* user, datapath_free_fn free)
	{
		if (!free)
			return nullptr;
		try {
			return std::make_shared<user_data>(user, free);
		} catch (...) {
			// The caller handed us ownership; honour it even when we fail.
			free(user);
			throw;
		}
	}

	// Copy a caller's option struct over the defaults, reading only the bytes the
	// caller says it has (older, smaller structs keep defaults for newer fields).
	template<typename T, void (*Init)(T*)>
	bool read_options(T& out, const T* in) noexcept
	{
		Init(&out);
		if (!in)
			return true;
		if (in->struct_size < sizeof(uint32_t))
			return false;
		std::memcpy(&out, in, in->struct_size < sizeof(T) ? in->struct_size : sizeof(T));
		out.struct_size = sizeof(T);
		return true;
	}

	bool convert(datapath::context_options& out, const datapath_context_options* in) noexcept
	{
		datapath_context_options o;
		if (!read_options<datapath_context_options, datapath_context_options_init>(o, in))
			return false;
		if (o.spin_us < 0 || o.priority < 0 || o.priority > 3 || o.ideal_core < -2)
			return false;
		out.spin       = std::chrono::microseconds(o.spin_us);
		out.io_threads = size_t(o.io_threads);
		out.priority   = o.priority;
		out.ideal_core = o.ideal_core;
		out.mmcss      = o.mmcss != 0;
		out.alertable  = o.alertable != 0;
		out.yield      = o.yield != 0;
		out.pair_peer  = o.pair_peer != 0;
		return true;
	}

	bool convert(datapath::options& out, const datapath_options* in) noexcept
	{
		datapath_options o;
		if (!read_options<datapath_options, datapath_options_init>(o, in))
			return false;
		if (o.permissions > DATAPATH_PERMISSIONS_WORLD || o.connect_timeout_ms < 0 || o.handshake_timeout_ms < 0)
			return false;
		out.max_message_size = o.max_message_size;
		out.max_queued_bytes = o.max_queued_bytes;
		out.permissions      = datapath::permissions(o.permissions);
		out.backlog          = size_t(o.backlog);
		out.max_clients      = size_t(o.max_clients);
		out.connect_timeout  = std::chrono::milliseconds(o.connect_timeout_ms);
		out.receive_buffer   = size_t(o.receive_buffer);
		out.pipe_buffer      = size_t(o.pipe_buffer);
		out.shared_memory           = o.shared_memory != 0;
		out.shared_memory_zero_copy = o.shared_memory_zero_copy != 0;
		out.shared_memory_size      = size_t(o.shared_memory_size);
		out.handshake_timeout       = std::chrono::milliseconds(o.handshake_timeout_ms);
		return true;
	}

	std::shared_ptr<datapath::context> context_of(datapath_context* c)
	{
		return c ? c->ctx : nullptr;
	}
} // namespace

extern "C" {

const char* datapath_error_string(datapath_error e)
{
	switch (e) {
	case DATAPATH_ERROR_INVALID_ARGUMENT:
		return "InvalidArgument";
	case DATAPATH_ERROR_OUT_OF_MEMORY:
		return "OutOfMemory";
	default:
		return datapath::to_string(error(e));
	}
}

uint32_t datapath_version(void)
{
#ifdef DP_HAS_VERSION_INFO
	auto v = datapath::version();
	return (uint32_t(v.major) << 24) | (uint32_t(v.minor) << 8) | uint32_t(v.patch);
#else
	return (uint32_t(PROJECT_VERSION_MAJOR) << 24) | (uint32_t(PROJECT_VERSION_MINOR) << 8)
		   | uint32_t(PROJECT_VERSION_PATCH);
#endif
}

uint32_t datapath_c_abi_version(void)
{
	return DATAPATH_C_ABI_VERSION;
}

void datapath_context_options_init(datapath_context_options* o)
{
	if (!o)
		return;
	datapath::context_options d;
	std::memset(o, 0, sizeof(*o));
	o->struct_size = sizeof(*o);
	o->priority    = d.priority;
	o->spin_us     = int64_t(d.spin.count());
	o->io_threads  = uint64_t(d.io_threads);
	o->ideal_core  = d.ideal_core;
	o->mmcss       = d.mmcss ? 1 : 0;
	o->alertable   = d.alertable ? 1 : 0;
	o->yield       = d.yield ? 1 : 0;
	o->pair_peer   = d.pair_peer ? 1 : 0;
}

void datapath_options_init(datapath_options* o)
{
	if (!o)
		return;
	datapath::options d;
	std::memset(o, 0, sizeof(*o));
	o->struct_size        = sizeof(*o);
	o->permissions        = uint32_t(d.permissions);
	o->max_message_size   = d.max_message_size;
	o->max_queued_bytes   = d.max_queued_bytes;
	o->backlog            = uint64_t(d.backlog);
	o->max_clients        = uint64_t(d.max_clients);
	o->connect_timeout_ms = int64_t(d.connect_timeout.count());
	o->receive_buffer     = uint64_t(d.receive_buffer);
	o->pipe_buffer        = uint64_t(d.pipe_buffer);
	o->shared_memory           = d.shared_memory ? 1 : 0;
	o->shared_memory_zero_copy = d.shared_memory_zero_copy ? 1 : 0;
	o->shared_memory_size      = uint64_t(d.shared_memory_size);
	o->handshake_timeout_ms    = int64_t(d.handshake_timeout.count());
}

// context ----------------------------------------------------------------------

datapath_error datapath_context_create(const datapath_context_options* options, datapath_context** out)
{
	if (!out)
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	datapath::context_options co;
	if (!convert(co, options))
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	return guarded([&]() -> datapath_error {
		auto ctx = datapath::context::create(co);
		if (!ctx)
			return DATAPATH_ERROR_CRITICAL_FAILURE;
		*out = new datapath_context{std::move(ctx)};
		return DATAPATH_SUCCESS;
	});
}

datapath_error datapath_context_get_default(datapath_context** out)
{
	if (!out)
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	return guarded([&]() -> datapath_error {
		auto ctx = datapath::context::get_default();
		if (!ctx)
			return DATAPATH_ERROR_CRITICAL_FAILURE;
		*out = new datapath_context{std::move(ctx)};
		return DATAPATH_SUCCESS;
	});
}

void datapath_context_release(datapath_context* c)
{
	delete c;
}

// socket -----------------------------------------------------------------------

datapath_error datapath_connect(const char* path, const datapath_options* options, datapath_context* context,
								datapath_socket** out)
{
	if (!path || !out)
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	datapath::options o;
	if (!convert(o, options))
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	return guarded([&]() -> datapath_error {
		std::unique_ptr<datapath_socket> handle(new datapath_socket);
		auto ec = datapath::connect(handle->sock, std::string(path), o, context_of(context));
		if (ec != error::Success)
			return to_c(ec);
		*out = handle.release();
		return DATAPATH_SUCCESS;
	});
}

datapath_error datapath_socket_set_on_message(datapath_socket* s, datapath_message_fn fn, void* user,
											  datapath_free_fn user_free)
{
	if (!s) {
		if (user_free)
			user_free(user);
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	}
	return guarded([&]() -> datapath_error {
		auto owner = own(user, user_free);
		if (!fn) {
			s->sock->on_message(nullptr);
			return DATAPATH_SUCCESS;
		}
		s->sock->on_message([fn, user, s, owner](const uint8_t* data, size_t size) { fn(user, s, data, size); });
		return DATAPATH_SUCCESS;
	});
}

datapath_error datapath_socket_set_on_close(datapath_socket* s, datapath_close_fn fn, void* user,
											datapath_free_fn user_free)
{
	if (!s) {
		if (user_free)
			user_free(user);
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	}
	return guarded([&]() -> datapath_error {
		auto owner = own(user, user_free);
		if (!fn) {
			s->sock->on_close(nullptr);
			return DATAPATH_SUCCESS;
		}
		s->sock->on_close([fn, user, s, owner](datapath::error reason) { fn(user, s, to_c(reason)); });
		return DATAPATH_SUCCESS;
	});
}

datapath_error datapath_socket_send(datapath_socket* s, const void* data, size_t size)
{
	if (!s || (!data && size))
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	return guarded([&]() { return to_c(s->sock->send(data, size)); });
}

datapath_error datapath_socket_sendv(datapath_socket* s, const datapath_buffer* parts, size_t count)
{
	if (!s || (!parts && count))
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	return guarded([&]() {
		return to_c(s->sock->sendv(reinterpret_cast<const datapath::buffer_view*>(parts), count));
	});
}

datapath_error datapath_socket_close(datapath_socket* s)
{
	if (!s)
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	return guarded([&]() { return to_c(s->sock->close()); });
}

int datapath_socket_good(const datapath_socket* s)
{
	return s && s->sock->good() ? 1 : 0;
}

uint64_t datapath_socket_max_message_size(const datapath_socket* s)
{
	return s ? s->sock->max_message_size() : 0;
}

int datapath_socket_set_zero_copy(datapath_socket* s, int enable)
{
	return s && s->sock->set_zero_copy(enable != 0) ? 1 : 0;
}

void datapath_socket_release(datapath_socket* s)
{
	delete s;
}

// server -----------------------------------------------------------------------

datapath_error datapath_host(const char* path, const datapath_options* options, datapath_context* context,
							 datapath_server** out)
{
	if (!path || !out)
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	datapath::options o;
	if (!convert(o, options))
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	return guarded([&]() -> datapath_error {
		std::unique_ptr<datapath_server> handle(new datapath_server);
		auto ec = datapath::host(handle->srv, std::string(path), o, context_of(context));
		if (ec != error::Success)
			return to_c(ec);
		*out = handle.release();
		return DATAPATH_SUCCESS;
	});
}

datapath_error datapath_server_set_on_accept(datapath_server* srv, datapath_accept_fn fn, void* user,
											 datapath_free_fn user_free)
{
	if (!srv) {
		if (user_free)
			user_free(user);
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	}
	return guarded([&]() -> datapath_error {
		auto owner = own(user, user_free);
		if (!fn) {
			srv->srv->on_accept(nullptr);
			return DATAPATH_SUCCESS;
		}
		srv->srv->on_accept([fn, user, srv, owner](std::shared_ptr<datapath::socket> client) noexcept {
			datapath_socket* handle = new (std::nothrow) datapath_socket{std::move(client)};
			if (!handle)
				return false;
			if (fn(user, srv, handle))
				return true;
			delete handle;
			return false;
		});
		return DATAPATH_SUCCESS;
	});
}

datapath_error datapath_server_close(datapath_server* srv)
{
	if (!srv)
		return DATAPATH_ERROR_INVALID_ARGUMENT;
	return guarded([&]() { return to_c(srv->srv->close()); });
}

void datapath_server_release(datapath_server* srv)
{
	delete srv;
}

} // extern "C"
