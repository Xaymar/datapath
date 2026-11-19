/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// macOS and iOS.

#include "platform.hpp"
#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <cerrno>

namespace {
	// Close-on-exec, non-blocking and no SIGPIPE. Closes `fd` on failure.
	int prepare(int fd)
	{
		if (fd < 0)
			return fd;
		int one = 1;
		if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) != 0
			|| ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
			int err = errno;
			::close(fd);
			errno = err;
			return -1;
		}
		return fd;
	}
} // namespace

int datapath::posix::open_stream_socket()
{
	return prepare(::socket(AF_UNIX, SOCK_STREAM, 0));
}

int datapath::posix::accept_stream_socket(int listener)
{
	return prepare(::accept(listener, nullptr, nullptr));
}

// Unix stream sockets start with 8 KiB buffers (net.local.stream.sendspace and
// recvspace), so a 64 KiB message can take several wake-ups of the reader. Raise
// both to options::pipe_buffer (or receive_buffer when that is 0), halving
// until the kernel accepts it: kern.ipc.maxsockbuf caps the size (8 MiB by
// default).
void datapath::posix::tune_socket_buffers(int fd, const datapath::options& opts)
{
	size_t want = opts.pipe_buffer ? opts.pipe_buffer : opts.receive_buffer;
	if (want > (size_t(1) << 30))
		want = size_t(1) << 30;
	for (int opt : {SO_SNDBUF, SO_RCVBUF}) {
		for (size_t size = want; size >= 8192; size /= 2) {
			int v = int(size);
			if (::setsockopt(fd, SOL_SOCKET, opt, &v, sizeof(v)) == 0)
				break;
		}
	}
}

bool datapath::posix::peer_credentials(int fd, uid_t& uid, gid_t& gid)
{
	return ::getpeereid(fd, &uid, &gid) == 0;
}

// The scheduler picks threads, cores (performance or efficiency) and timer
// coalescing by quality-of-service class rather than by priority: 1 =
// user-initiated, 2 and 3 = user-interactive, the class of a UI's main thread.
// Unlike nice values, these need no privileges. Real-time (time constraint)
// scheduling is not used: a spinning real-time thread is demoted by the kernel
// once it overruns its computation budget, which would show up as a tail stall.
void datapath::posix::apply_thread_priority(int level)
{
	qos_class_t cls;
	switch (level) {
	case 1:
		cls = QOS_CLASS_USER_INITIATED;
		break;
	case 2:
	case 3:
		cls = QOS_CLASS_USER_INTERACTIVE;
		break;
	default:
		return;
	}
	::pthread_set_qos_class_self_np(cls, 0);
}

size_t datapath::posix::physical_core_count()
{
	int    n   = 0;
	size_t len = sizeof(n);
	if (::sysctlbyname("hw.physicalcpu", &n, &len, nullptr, 0) != 0 || n <= 0)
		return 0;
	return size_t(n);
}
