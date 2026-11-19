/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

/* Compiled as C: proves datapath.h is valid C and runs an echo round trip
 * using nothing but the C API. Called from test_c_api.cpp. */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif
#include <datapath/datapath.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* The reply flag crosses threads. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
typedef atomic_int flag_t;
#define FLAG_SET(f) atomic_store(&(f), 1)
#define FLAG_GET(f) atomic_load(&(f))
#elif defined(_WIN32)
typedef volatile LONG flag_t;
#define FLAG_SET(f) InterlockedExchange(&(f), 1)
#define FLAG_GET(f) InterlockedCompareExchange(&(f), 0, 0)
#else
typedef volatile int flag_t;
#define FLAG_SET(f) ((f) = 1)
#define FLAG_GET(f) (f)
#endif

int c_api_echo_round_trip(const char* path);

#ifdef _WIN32
static void sleep_ms(unsigned ms)
{
	Sleep(ms);
}
#else
#include <time.h>
static void sleep_ms(unsigned ms)
{
	struct timespec ts;
	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}
#endif

#define MAX_CLIENTS 4

typedef struct server_state {
	datapath_socket* clients[MAX_CLIENTS];
	int              count;
} server_state;

typedef struct client_state {
	char   received[64];
	flag_t done;
} client_state;

static void echo(void* user, datapath_socket* socket, const uint8_t* data, size_t size)
{
	(void)user;
	datapath_socket_send(socket, data, size);
}

static int accept_client(void* user, datapath_server* server, datapath_socket* client)
{
	server_state* st = (server_state*)user;
	(void)server;
	if (st->count >= MAX_CLIENTS)
		return 0;
	datapath_socket_set_on_message(client, echo, NULL, NULL);
	st->clients[st->count] = client;
	st->count++;
	return 1;
}

static void on_reply(void* user, datapath_socket* socket, const uint8_t* data, size_t size)
{
	client_state* st = (client_state*)user;
	(void)socket;
	if (size >= sizeof(st->received))
		size = sizeof(st->received) - 1;
	memcpy(st->received, data, size);
	st->received[size] = '\0';
	FLAG_SET(st->done);
}

/* Returns 0 on success, or the step number that failed. */
int c_api_echo_round_trip(const char* path)
{
	server_state     srv_state;
	client_state     cli_state;
	datapath_server* server = NULL;
	datapath_socket* client = NULL;
	datapath_buffer  parts[2];
	int              result = 0, i;

	memset(&srv_state, 0, sizeof(srv_state));
	memset(&cli_state, 0, sizeof(cli_state));

	if (datapath_host(path, NULL, NULL, &server) != DATAPATH_SUCCESS)
		return 1;
	if (datapath_server_set_on_accept(server, accept_client, &srv_state, NULL) != DATAPATH_SUCCESS) {
		result = 2;
		goto out;
	}
	if (datapath_connect(path, NULL, NULL, &client) != DATAPATH_SUCCESS) {
		result = 3;
		goto out;
	}
	datapath_socket_set_on_message(client, on_reply, &cli_state, NULL);

	parts[0].data = "hello, ";
	parts[0].size = 7;
	parts[1].data = "c";
	parts[1].size = 1;
	if (datapath_socket_sendv(client, parts, 2) != DATAPATH_SUCCESS) {
		result = 4;
		goto out;
	}
	for (i = 0; i < 5000 && !FLAG_GET(cli_state.done); i++)
		sleep_ms(1);
	if (!FLAG_GET(cli_state.done)) {
		result = 5;
		goto out;
	}
	if (strcmp(cli_state.received, "hello, c") != 0)
		result = 6;

out:
	datapath_socket_release(client);
	datapath_server_release(server);
	for (i = 0; i < srv_state.count; i++)
		datapath_socket_release(srv_state.clients[i]);
	return result;
}
