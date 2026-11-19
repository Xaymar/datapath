/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Linux and Android.

#include "platform.hpp"
#include <sched.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>

int datapath::posix::open_stream_socket()
{
	return ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

int datapath::posix::accept_stream_socket(int listener)
{
	return ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

void datapath::posix::tune_socket_buffers(int, const datapath::options&)
{
	// The Linux defaults (~208 KiB) are left as they are.
}

bool datapath::posix::peer_credentials(int fd, uid_t& uid, gid_t& gid)
{
	ucred     cred{};
	socklen_t len = sizeof(cred);
	if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
		return false;
	uid = cred.uid;
	gid = cred.gid;
	return true;
}

// Raise the calling thread's priority by lowering its nice value, relative to
// the nice value it inherited (like Windows thread priorities are relative to
// the process's priority class). The default level 2 needs a large gap: the
// scheduler shares CPU by weight, and against a full load at the same nice
// value 64 B p99.9 was ~8 ms at a gap of 0, ~4 ms at 8, ~200 us at 12 and
// ~160 us at 19. Level 3 matches Android's THREAD_PRIORITY_URGENT_AUDIO.
// Unprivileged processes may only go down to 20 - RLIMIT_NICE; the thread then
// gets the lowest nice value it is allowed, or stays where it was.
// Real-time policies (SCHED_FIFO/RR) are not used: a spinning real-time thread
// can starve the rest of the system.
void datapath::posix::apply_thread_priority(int level)
{
	static const int steps[] = {0, -4, -12, -19};
	if (level <= 0 || level > 3)
		return;
	pid_t tid = ::gettid(); // Linux: PRIO_PROCESS with a thread id affects only that thread
	errno     = 0;
	int base  = ::getpriority(PRIO_PROCESS, id_t(tid));
	if (base == -1 && errno != 0)
		return;
	int want = std::max(-20, base + steps[level]);
	if (::setpriority(PRIO_PROCESS, id_t(tid), want) == 0)
		return;
	rlimit rl{};
	if (::getrlimit(RLIMIT_NICE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY)
		return;
	int floor = 20 - int(std::min<rlim_t>(rl.rlim_cur, 40));
	if (floor > want && floor < base)
		::setpriority(PRIO_PROCESS, id_t(tid), floor);
}

// CPUs in the affinity mask that are the first hyper-thread of their core.
size_t datapath::posix::physical_core_count()
{
	cpu_set_t set;
	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) != 0)
		return 0;
	size_t n = 0;
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &set))
			continue;
		char path[96];
		std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
		FILE* f = std::fopen(path, "r");
		if (!f)
			return 0;
		int  sibling = -1;
		bool ok      = std::fscanf(f, "%d", &sibling) == 1;
		std::fclose(f);
		if (!ok)
			return 0;
		// The first sibling counts for the core, unless it is outside the mask.
		if (sibling == cpu || !CPU_ISSET(sibling, &set))
			n++;
	}
	return n;
}
