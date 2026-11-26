/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <sys/socket.h>
#include <sys/types.h>
#include <cstddef>
#include <cstdint>
#include <datapath/options.hpp>

// The parts of the POSIX backend that differ between operating systems. Each
// OS implements these in its own platform_<os>.cpp.
namespace datapath { namespace posix {
	// Readiness bits passed to fd_target::on_event() and to event_loop
	// registrations, independent of the poller (epoll or kqueue).
	namespace ev {
		enum : uint32_t {
			in    = 1u << 0, // readable
			out   = 1u << 1, // writable
			rdhup = 1u << 2, // peer shut down its writing side
			hup   = 1u << 3, // hung up
			err   = 1u << 4, // error condition
		};
	}

	// Flags for every send(). Where the OS has no MSG_NOSIGNAL, sockets get
	// SO_NOSIGPIPE when they are created instead.
#ifdef MSG_NOSIGNAL
	constexpr int send_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
	constexpr int send_flags = MSG_DONTWAIT;
#endif

	// A non-blocking, close-on-exec AF_UNIX stream socket that never raises
	// SIGPIPE. -1 with errno set on failure.
	int open_stream_socket();
	// accept() on `listener`, with the same properties. -1 with errno set;
	// ECONNABORTED means a connection was dropped and the next one may wait.
	int accept_stream_socket(int listener);
	// Apply per-connection kernel buffer sizes, where the OS defaults are too
	// small for the library's traffic (macOS: 8 KiB). No-op elsewhere.
	void tune_socket_buffers(int fd, const datapath::options& opts);
	// Credentials of the process on the other end of a connected socket.
	bool peer_credentials(int fd, uid_t& uid, gid_t& gid);

	// Raise the calling thread's scheduling priority to `level` (0..3, see
	// context_options::priority).
	void apply_thread_priority(int level);
	// Physical cores available to this process, 0 if unknown.
	size_t physical_core_count();
}} // namespace datapath::posix
