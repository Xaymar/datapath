/*
 * Low Latency IPC Library for high-speed traffic
 * Copyright (C) 2017-2019 Michael Fabian Dirks <info@xaymar.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#pragma once
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include "error.hpp"

namespace datapath {
	namespace threadpool {
		typedef uint64_t affinity_t;

		constexpr affinity_t default_mask = std::numeric_limits<affinity_t>::max();

		struct task {
			std::function<void()> function;
			affinity_t            mask = default_mask;
		};

		class pool {
			struct worker {
				affinity_t affinity;
				bool       should_stop = false;

				std::thread thread;

				std::mutex                        mutex;
				std::queue<std::shared_ptr<task>> queue;
				std::condition_variable           signal;

				worker(affinity_t affinity);
				~worker();

				void runner();

				void clear();

				void push(std::shared_ptr<task> task);
			};

			std::map<affinity_t, std::shared_ptr<worker>> _workers;

			public:
			pool();
			~pool();

			bool push(std::shared_ptr<task> task);

			void clear(affinity_t mask = default_mask);
		};
	} // namespace threadpool
} // namespace datapath
