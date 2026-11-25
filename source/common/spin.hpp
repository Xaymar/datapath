/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <chrono>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define DATAPATH_CPU_RELAX() _mm_pause()
#elif defined(_M_ARM64) || defined(_M_ARM)
#include <intrin.h>
#define DATAPATH_CPU_RELAX() __yield()
#elif defined(__aarch64__) || defined(__arm__)
#define DATAPATH_CPU_RELAX() __asm__ __volatile__("yield")
#else
#define DATAPATH_CPU_RELAX() ((void)0)
#endif

namespace datapath { namespace detail {
	struct cpu_relax {
		void operator()() const noexcept
		{
			DATAPATH_CPU_RELAX();
		}
	};

	// Busy-poll with `poll()` until `budget` passes without work, then call `block()`
	// once. poll() returns true when it handled work, which restarts the budget.
	// `relax()` runs between idle polls.
	template<typename Poll, typename Block, typename Relax = cpu_relax>
	void spin_then_block(std::chrono::microseconds budget, Poll&& poll, Block&& block, Relax&& relax = Relax())
	{
		using clock = std::chrono::steady_clock;
		if (budget.count() > 0) {
			auto deadline = clock::now() + budget;
			for (;;) {
				if (poll()) {
					deadline = clock::now() + budget;
					continue;
				}
				if (clock::now() >= deadline)
					break;
				relax();
			}
		}
		block();
	}
}} // namespace datapath::detail
