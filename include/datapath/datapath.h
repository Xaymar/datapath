/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

/* C API for DataPath. A thin, stable ABI over the C++ library, meant for C
 * programs and for other languages' FFI (Rust, Python, C#, ...).
 *
 * Conventions
 * - Every object is an opaque handle created by a datapath_*_create/connect/
 *   host call and destroyed with the matching datapath_*_release call.
 * - Functions that can fail return a datapath_error (0 = DATAPATH_SUCCESS) and
 *   write results through out-parameters. Out-parameters are left untouched on
 *   failure. No C++ exception ever crosses this boundary.
 * - Callbacks run on an I/O thread and receive the `user` pointer given when
 *   they were set. An optional `user_free` function is called exactly once when
 *   the library drops that callback: it was replaced, the connection ended, or
 *   the handle was released. `user_free` must not call back into DataPath.
 * - Callbacks must not unwind (no C++ exceptions, no Rust panics) into DataPath.
 * - All functions are thread-safe, except that a handle must not be used by any
 *   other thread while or after it is released.
 * - Option structs carry their own size (`struct_size`): initialise them with
 *   the matching *_init function, then change fields. This lets later versions
 *   add fields without breaking binaries built against this header. Passing
 *   NULL instead of an options struct means "use the defaults".
 * - Fields use fixed-width types, so the layout is the same on every platform
 *   of a given pointer size.
 */

#ifndef DATAPATH_DATAPATH_H
#define DATAPATH_DATAPATH_H

#include <stddef.h>
#include <stdint.h>

/* Symbol visibility: the same switches as the C++ API (datapath/export.h). */
#include <datapath/export.h>
#define DATAPATH_C_API DATAPATH_API

#ifdef __cplusplus
extern "C" {
#endif

/* Version of this C interface, separate from the C++ library's ABI version:
 * bumped only when this header changes in a way that breaks binaries built
 * against an older one. Compare with datapath_c_abi_version() at run time. */
#define DATAPATH_C_ABI_VERSION 1

/* Errors ------------------------------------------------------------------- */

typedef int32_t datapath_error;

/* Values match datapath::error in the C++ API. */
enum {
	DATAPATH_ERROR_UNKNOWN          = -1,
	DATAPATH_SUCCESS                = 0,
	/* Generic failure; details were not mappable to a specific code. */
	DATAPATH_ERROR_FAILURE          = 1,
	DATAPATH_ERROR_CRITICAL_FAILURE = 2,
	/* The connection or server is closed. */
	DATAPATH_ERROR_CLOSED           = 3,
	DATAPATH_ERROR_TIMED_OUT        = 4,
	/* The path is malformed or too long for the platform. */
	DATAPATH_ERROR_INVALID_PATH     = 5,
	DATAPATH_ERROR_NOT_SUPPORTED    = 6,
	/* Message exceeds the negotiated maximum message size. */
	DATAPATH_ERROR_TOO_LARGE        = 7,
	/* The send queue is full; retry later. */
	DATAPATH_ERROR_WOULD_BLOCK      = 8,
	/* The peer speaks a different protocol version. */
	DATAPATH_ERROR_VERSION_MISMATCH = 9,
	/* Nothing is listening at the path. */
	DATAPATH_ERROR_NOT_FOUND        = 10,
	/* Another server already hosts this path. */
	DATAPATH_ERROR_ADDRESS_IN_USE   = 11,
	DATAPATH_ERROR_ACCESS_DENIED    = 12,
	/* A required pointer was NULL or a value was out of range. */
	DATAPATH_ERROR_INVALID_ARGUMENT = 13,
	/* Memory allocation failed. */
	DATAPATH_ERROR_OUT_OF_MEMORY    = 14,
};

/* Static string naming the error, e.g. "WouldBlock". Never NULL. */
DATAPATH_C_API const char* datapath_error_string(datapath_error error);

/* Version ------------------------------------------------------------------ */

/* Version of the loaded library as (abi << 16) | patch, the same encoding as
 * DATAPATH_VERSION in <datapath/version.hpp>. */
DATAPATH_C_API uint32_t datapath_version(void);
/* DATAPATH_C_ABI_VERSION of the library that is actually loaded. */
DATAPATH_C_API uint32_t datapath_c_abi_version(void);

/* Handles ------------------------------------------------------------------ */

/* Owns the I/O threads. Releasing the last handle to a context closes every
 * socket and server on it. */
typedef struct datapath_context datapath_context;
/* One end of a connection. Releasing it closes the connection. */
typedef struct datapath_socket datapath_socket;
/* A hosted server. Releasing it stops accepting; accepted sockets stay open. */
typedef struct datapath_server datapath_server;

/* Options ------------------------------------------------------------------ */

/* Who may connect to a hosted server. */
enum {
	/* Only the user that created the server. */
	DATAPATH_PERMISSIONS_USER  = 0,
	/* The creating user and the local user group. */
	DATAPATH_PERMISSIONS_GROUP = 1,
	/* Everyone on the local machine. */
	DATAPATH_PERMISSIONS_WORLD = 2,
};

/* See datapath::context_options in options.hpp for what each field does. */
typedef struct datapath_context_options {
	/* sizeof(datapath_context_options); set by datapath_context_options_init. */
	uint32_t struct_size;
	/* I/O thread priority: 0 normal .. 3 time critical. Default 2. */
	int32_t  priority;
	/* Busy-poll time of an idle I/O thread, in microseconds. Default 50. */
	int64_t  spin_us;
	/* Number of I/O threads, 0 = one per physical core. Default 1. */
	uint64_t io_threads;
	/* Windows only: -1 automatic (default), -2 off, N >= 0 first core. */
	int32_t  ideal_core;
	/* Windows only: register with MMCSS. Default 0. */
	uint8_t  mmcss;
	/* Windows only: alertable waits. Default 0. */
	uint8_t  alertable;
	/* Yield after inline writes and while spinning. Default 0. */
	uint8_t  yield;
	/* Windows only: pair a client and server that both set this on one physical
	 * core (see context_options::pair_peer). Default 0. Was reserved, so older
	 * callers that zeroed it keep the default. */
	uint8_t  pair_peer;
} datapath_context_options;

/* See datapath::options in options.hpp for what each field does. */
typedef struct datapath_options {
	/* sizeof(datapath_options); set by datapath_options_init. */
	uint32_t struct_size;
	/* DATAPATH_PERMISSIONS_*; host only. Default USER. */
	uint32_t permissions;
	/* Largest message accepted. Default 64 MiB. */
	uint64_t max_message_size;
	/* Bytes that may wait in the send queue. Default 64 MiB. */
	uint64_t max_queued_bytes;
	/* Pending connection slots kept ready; host only. Default 8. */
	uint64_t backlog;
	/* Maximum connected clients, 0 = unlimited; host only. Default 0. */
	uint64_t max_clients;
	/* How long connect waits for the handshake, in milliseconds. Default 1000. */
	int64_t  connect_timeout_ms;
	/* Receive buffer per connection. Default 256 KiB. */
	uint64_t receive_buffer;
	/* Windows kernel pipe buffer, 0 = receive_buffer; host only. Default 1 MiB. */
	uint64_t pipe_buffer;
} datapath_options;

/* Fill with the library defaults and set struct_size. */
DATAPATH_C_API void datapath_context_options_init(datapath_context_options* options);
DATAPATH_C_API void datapath_options_init(datapath_options* options);

/* Context ------------------------------------------------------------------ */

/* Create a context with its own I/O threads. `options` may be NULL. */
DATAPATH_C_API datapath_error datapath_context_create(const datapath_context_options* options,
													  datapath_context**              out_context);
/* A new handle to the process-wide default context. Release it like any other. */
DATAPATH_C_API datapath_error datapath_context_get_default(datapath_context** out_context);
/* Drop this handle. NULL is ignored. */
DATAPATH_C_API void           datapath_context_release(datapath_context* context);

/* Socket ------------------------------------------------------------------- */

/* One part of a message for datapath_socket_sendv. */
typedef struct datapath_buffer {
	const void* data;
	size_t      size;
} datapath_buffer;

/* A message arrived. `data` is valid only for the duration of the call. */
typedef void (*datapath_message_fn)(void* user, datapath_socket* socket, const uint8_t* data, size_t size);
/* The connection ended. Called at most once, and never after the socket was
 * released. `reason` is DATAPATH_ERROR_CLOSED for an orderly close. */
typedef void (*datapath_close_fn)(void* user, datapath_socket* socket, datapath_error reason);
/* Destroys a callback's user data. */
typedef void (*datapath_free_fn)(void* user);

/* Connect to a server at `path` (UTF-8, NUL-terminated). Blocks until the
 * handshake completes or options->connect_timeout_ms elapses. `options` and
 * `context` may be NULL (defaults / default context). */
DATAPATH_C_API datapath_error datapath_connect(const char* path, const datapath_options* options,
											   datapath_context* context, datapath_socket** out_socket);

/* Set the message callback; NULL `callback` clears it. The first non-NULL
 * callback starts receiving; messages that arrive earlier wait in the kernel.
 * Until then the library does not read, so it also does not notice the peer
 * closing: set a message callback to learn about remote closes.
 * The message and close callbacks of one socket never run concurrently with
 * each other or with themselves, and messages arrive in the order sent.
 * Callbacks of different sockets may run concurrently. */
DATAPATH_C_API datapath_error datapath_socket_set_on_message(datapath_socket* socket, datapath_message_fn callback,
															 void* user, datapath_free_fn user_free);
/* Set the close callback; NULL `callback` clears it. */
DATAPATH_C_API datapath_error datapath_socket_set_on_close(datapath_socket* socket, datapath_close_fn callback,
														   void* user, datapath_free_fn user_free);

/* Queue one message. Never blocks, safe from callbacks. Returns
 * DATAPATH_ERROR_TOO_LARGE, _WOULD_BLOCK or _CLOSED when not queued. */
DATAPATH_C_API datapath_error datapath_socket_send(datapath_socket* socket, const void* data, size_t size);
/* Queue one message assembled from `count` parts. */
DATAPATH_C_API datapath_error datapath_socket_sendv(datapath_socket* socket, const datapath_buffer* parts,
													size_t count);

/* Close the connection; the close callback fires. The handle stays valid
 * until released. */
DATAPATH_C_API datapath_error datapath_socket_close(datapath_socket* socket);
/* 1 while the connection is open, 0 otherwise (or for NULL). */
DATAPATH_C_API int            datapath_socket_good(const datapath_socket* socket);
/* Negotiated message size limit of this connection (0 for NULL). */
DATAPATH_C_API uint64_t       datapath_socket_max_message_size(const datapath_socket* socket);
/* Close, wait for running callbacks, and free the handle. No callback fires
 * afterwards (not even the close callback); user data is freed. May be called
 * from the socket's own callbacks. NULL is ignored. */
DATAPATH_C_API void           datapath_socket_release(datapath_socket* socket);

/* Server ------------------------------------------------------------------- */

/* A client passed the handshake. Return non-zero to accept: `client` then
 * belongs to the callee, who must eventually datapath_socket_release it.
 * Return 0 to reject: the library releases `client`, and the client's connect
 * returns DATAPATH_ERROR_CLOSED. */
typedef int (*datapath_accept_fn)(void* user, datapath_server* server, datapath_socket* client);

/* Host a server at `path` (UTF-8, NUL-terminated). Set the accept callback
 * before clients connect, or they wait. `options` and `context` may be NULL. */
DATAPATH_C_API datapath_error datapath_host(const char* path, const datapath_options* options,
											datapath_context* context, datapath_server** out_server);
/* Set the accept callback; NULL `callback` clears it. */
DATAPATH_C_API datapath_error datapath_server_set_on_accept(datapath_server* server, datapath_accept_fn callback,
															void* user, datapath_free_fn user_free);
/* Stop accepting. The handle stays valid until released. */
DATAPATH_C_API datapath_error datapath_server_close(datapath_server* server);
/* Close, wait for a running accept callback, and free the handle. NULL is
 * ignored. */
DATAPATH_C_API void           datapath_server_release(datapath_server* server);

#ifdef __cplusplus
}
#endif

#endif /* DATAPATH_DATAPATH_H */
