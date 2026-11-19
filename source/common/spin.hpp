/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
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

namespace datapath::detail {
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
} // namespace datapath::detail
