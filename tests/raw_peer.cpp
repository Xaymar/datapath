/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "raw_peer.hpp"
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct raw::peer {
	HANDLE h;
};

void raw::peer_deleter::operator()(peer* p) const
{
	CloseHandle(p->h);
	delete p;
}

raw::peer_ptr raw::connect(const std::string& name)
{
	std::string  full = "\\\\.\\pipe\\" + name;
	std::wstring w(full.begin(), full.end());
	for (int i = 0; i < 100; i++) {
		HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h != INVALID_HANDLE_VALUE)
			return peer_ptr(new peer{h});
		if (GetLastError() != ERROR_PIPE_BUSY)
			return nullptr;
		WaitNamedPipeW(w.c_str(), 100);
	}
	return nullptr;
}

bool raw::write(peer& p, const void* data, size_t size)
{
	DWORD done = 0;
	return WriteFile(p.h, data, DWORD(size), &done, nullptr) && done == size;
}

static bool wait_readable(HANDLE h, size_t want, int timeout_ms, bool& closed)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	closed        = false;
	for (;;) {
		DWORD avail = 0;
		if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
			closed = true;
			return false;
		}
		if (avail >= want)
			return true;
		if (std::chrono::steady_clock::now() > deadline)
			return false;
		Sleep(1);
	}
}

bool raw::read(peer& p, void* data, size_t size, int timeout_ms)
{
	bool closed;
	if (!wait_readable(p.h, size, timeout_ms, closed))
		return false;
	DWORD done = 0;
	return ReadFile(p.h, data, DWORD(size), &done, nullptr) && done == size;
}

bool raw::wait_closed(peer& p, int timeout_ms)
{
	// Drain whatever the server wrote until the pipe reports it is broken.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	for (;;) {
		DWORD avail = 0;
		if (!PeekNamedPipe(p.h, nullptr, 0, nullptr, &avail, nullptr))
			return true;
		if (avail > 0) {
			char  buf[4096];
			DWORD got = 0;
			if (!ReadFile(p.h, buf, sizeof(buf) < avail ? DWORD(sizeof(buf)) : avail, &got, nullptr))
				return true;
			continue;
		}
		if (std::chrono::steady_clock::now() > deadline)
			return false;
		Sleep(1);
	}
}

#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

struct raw::peer {
	int fd;
};

void raw::peer_deleter::operator()(peer* p) const
{
	::close(p->fd);
	delete p;
}

raw::peer_ptr raw::connect(const std::string& name)
{
	int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return nullptr;
	sockaddr_un addr{};
	addr.sun_family     = AF_UNIX;
	std::string abs     = std::string("datapath.") + name;
	std::memcpy(addr.sun_path + 1, abs.data(), abs.size());
	socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + abs.size());
	if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
		::close(fd);
		return nullptr;
	}
	return peer_ptr(new peer{fd});
}

bool raw::write(peer& p, const void* data, size_t size)
{
	const char* c = static_cast<const char*>(data);
	while (size) {
		ssize_t n = ::send(p.fd, c, size, MSG_NOSIGNAL);
		if (n <= 0)
			return false;
		c += n;
		size -= size_t(n);
	}
	return true;
}

bool raw::read(peer& p, void* data, size_t size, int timeout_ms)
{
	char* c        = static_cast<char*>(data);
	auto  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (size) {
		int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
		if (left < 0)
			return false;
		pollfd pfd{p.fd, POLLIN, 0};
		if (::poll(&pfd, 1, left) <= 0)
			return false;
		ssize_t n = ::recv(p.fd, c, size, 0);
		if (n <= 0)
			return false;
		c += n;
		size -= size_t(n);
	}
	return true;
}

bool raw::wait_closed(peer& p, int timeout_ms)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	for (;;) {
		int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
		if (left < 0)
			return false;
		pollfd pfd{p.fd, POLLIN, 0};
		if (::poll(&pfd, 1, left) <= 0)
			return false;
		char    buf[4096];
		ssize_t n = ::recv(p.fd, buf, sizeof(buf), 0);
		if (n <= 0)
			return true;
	}
}
#endif
