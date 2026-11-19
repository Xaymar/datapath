/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Monotonic nanosecond clock: QueryPerformanceCounter on Windows,
// CLOCK_MONOTONIC elsewhere.

#pragma once
#include <cstdint>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

inline int64_t now_ns()
{
#ifdef _WIN32
	static const int64_t freq = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return int64_t(f.QuadPart);
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	// Split to avoid overflow for long uptimes.
	return (c.QuadPart / freq) * 1000000000 + (c.QuadPart % freq) * 1000000000 / freq;
#else
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
}
