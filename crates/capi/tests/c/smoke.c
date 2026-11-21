/* Low Latency IPC Library for high-speed traffic
 * Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Compiled against the real include/datapath/datapath.h and linked with the
 * Rust library: checks the struct layouts the header promises and runs an
 * echo round trip through the C ABI. */

#include <datapath/datapath.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond)                                                                    \
	do {                                                                               \
		if (!(cond)) {                                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);   \
			return 1;                                                                  \
		}                                                                              \
	} while (0)

_Static_assert(sizeof(datapath_context_options) == 32, "context options size");
_Static_assert(offsetof(datapath_context_options, priority) == 4, "priority");
_Static_assert(offsetof(datapath_context_options, spin_us) == 8, "spin_us");
_Static_assert(offsetof(datapath_context_options, io_threads) == 16, "io_threads");
_Static_assert(offsetof(datapath_context_options, ideal_core) == 24, "ideal_core");
_Static_assert(offsetof(datapath_context_options, mmcss) == 28, "mmcss");
_Static_assert(sizeof(datapath_options) == 64, "options size");
_Static_assert(offsetof(datapath_options, permissions) == 4, "permissions");
_Static_assert(offsetof(datapath_options, max_message_size) == 8, "max_message_size");
_Static_assert(offsetof(datapath_options, backlog) == 24, "backlog");
_Static_assert(offsetof(datapath_options, connect_timeout_ms) == 40, "connect_timeout_ms");
_Static_assert(offsetof(datapath_options, pipe_buffer) == 56, "pipe_buffer");
_Static_assert(sizeof(datapath_buffer) == 2 * sizeof(void*), "buffer size");

static volatile int received = 0;
static char         last[64];
static volatile int freed   = 0;
static volatile int closed  = 0;

static void on_message(void* user, datapath_socket* socket, const uint8_t* data, size_t size)
{
	(void)user;
	if (size < sizeof(last)) {
		memcpy(last, data, size);
		last[size] = 0;
	}
	received++;
	(void)socket;
}

static void echo_message(void* user, datapath_socket* socket, const uint8_t* data, size_t size)
{
	(void)user;
	datapath_socket_send(socket, data, size);
}

static void on_close(void* user, datapath_socket* socket, datapath_error reason)
{
	(void)user;
	(void)socket;
	(void)reason;
	closed++;
}

static void free_user(void* user)
{
	(void)user;
	freed++;
}

static datapath_socket* accepted = NULL;

static int on_accept(void* user, datapath_server* server, datapath_socket* client)
{
	(void)user;
	(void)server;
	datapath_socket_set_on_message(client, echo_message, NULL, free_user);
	accepted = client;
	return 1;
}

int main(void)
{
	datapath_context_options co;
	datapath_context_options_init(&co);
	CHECK(co.struct_size == sizeof(co));
	CHECK(co.priority == 2);
	CHECK(co.spin_us == 50);
	CHECK(co.io_threads == 1);
	datapath_options o;
	datapath_options_init(&o);
	CHECK(o.struct_size == sizeof(o));
	CHECK(o.max_message_size == (64ull << 20));
	CHECK(o.connect_timeout_ms == 1000);

	CHECK(datapath_c_abi_version() == DATAPATH_C_ABI_VERSION);
	CHECK(strcmp(datapath_error_string(DATAPATH_ERROR_WOULD_BLOCK), "WouldBlock") == 0);
	CHECK(strcmp(datapath_error_string(DATAPATH_ERROR_INVALID_ARGUMENT), "InvalidArgument") == 0);

	datapath_context* ctx = NULL;
	CHECK(datapath_context_create(&co, &ctx) == DATAPATH_SUCCESS);
	CHECK(datapath_context_create(&co, NULL) == DATAPATH_ERROR_INVALID_ARGUMENT);

	char path[64];
	snprintf(path, sizeof(path), "c-smoke-%d", (int)getpid());

	datapath_server* server = NULL;
	CHECK(datapath_host(path, &o, ctx, &server) == DATAPATH_SUCCESS);
	CHECK(datapath_server_set_on_accept(server, on_accept, NULL, NULL) == DATAPATH_SUCCESS);

	datapath_socket* client = NULL;
	CHECK(datapath_connect(path, &o, ctx, &client) == DATAPATH_SUCCESS);
	CHECK(datapath_socket_good(client) == 1);
	CHECK(datapath_socket_max_message_size(client) == (64ull << 20));
	CHECK(datapath_socket_set_on_message(client, on_message, NULL, NULL) == DATAPATH_SUCCESS);
	CHECK(datapath_socket_set_on_close(client, on_close, NULL, NULL) == DATAPATH_SUCCESS);

	CHECK(datapath_socket_send(client, "hello", 5) == DATAPATH_SUCCESS);
	for (int i = 0; i < 2000 && received < 1; i++)
		usleep(1000);
	CHECK(received == 1);
	CHECK(strcmp(last, "hello") == 0);

	datapath_buffer parts[2] = {{"foo", 3}, {"bar", 3}};
	CHECK(datapath_socket_sendv(client, parts, 2) == DATAPATH_SUCCESS);
	for (int i = 0; i < 2000 && received < 2; i++)
		usleep(1000);
	CHECK(received == 2);
	CHECK(strcmp(last, "foobar") == 0);

	/* A null buffer with a size is invalid; the library never reads it. */
	CHECK(datapath_socket_send(client, NULL, 4) == DATAPATH_ERROR_INVALID_ARGUMENT);
	CHECK(datapath_socket_send(NULL, "x", 1) == DATAPATH_ERROR_INVALID_ARGUMENT);

	CHECK(datapath_socket_close(client) == DATAPATH_SUCCESS);
	CHECK(datapath_socket_close(client) == DATAPATH_ERROR_CLOSED);
	CHECK(datapath_socket_good(client) == 0);
	CHECK(datapath_socket_send(client, "x", 1) == DATAPATH_ERROR_CLOSED);
	for (int i = 0; i < 2000 && closed < 1; i++)
		usleep(1000);
	CHECK(closed == 1);

	datapath_socket_release(client);
	CHECK(datapath_socket_good(NULL) == 0);
	datapath_server_release(server);
	/* The accepted socket's callback data is freed once the connection is gone. */
	for (int i = 0; i < 2000 && freed < 1; i++)
		usleep(1000);
	datapath_socket_release(accepted);
	CHECK(freed == 1);
	datapath_context_release(ctx);
	puts("ok");
	return 0;
}
