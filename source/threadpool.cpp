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

#include "threadpool.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

datapath::threadpool::pool::worker::worker(affinity_t affinity) : affinity(affinity), should_stop(false)
{
	std::unique_lock<std::mutex> qlock(this->mutex);
	this->thread = std::thread(&datapath::threadpool::pool::worker::runner, this);
}

datapath::threadpool::pool::worker::~worker()
{
	this->should_stop = true;
	this->signal.notify_all();
	if (this->thread.joinable()) {
		this->thread.join();
	}
}

void datapath::threadpool::pool::worker::runner()
{
	std::shared_ptr<datapath::threadpool::task> my_task;

	// Assign affinity
#ifdef _WIN32
	SetThreadAffinityMask(reinterpret_cast<HANDLE>(this->thread.native_handle()), this->affinity);
#else
#endif

	while (!this->should_stop) {
		{ // Grab any available work.
			std::unique_lock<std::mutex> lock(this->mutex);
			if (this->queue.size() > 0)
				my_task = this->queue.front();
		}

		if (my_task) // Execute work if we have any.
			if (my_task->function)
				my_task->function();

		{ // Remove it from the queue and wait.
			std::unique_lock<std::mutex> slock(this->mutex);
			if (my_task) {
				this->queue.pop();
				my_task.reset();
			}
			if (this->queue.size() == 0) {
				this->signal.wait(slock, [this]() { return (this->should_stop) || (this->queue.size() > 0); });
			}
		}
	}
}

void datapath::threadpool::pool::worker::clear()
{
	std::unique_lock<std::mutex> slock(this->mutex);
	while (this->queue.size() > 0)
		this->queue.pop();
}

void datapath::threadpool::pool::worker::push(std::shared_ptr<task> task)
{
	{
		std::unique_lock<std::mutex> slock(this->mutex);
		this->queue.push(task);
	}
	this->signal.notify_all();
}

datapath::threadpool::pool::pool()
{
	// Spawn x number of threads for working.
	uint64_t num_hw_concurrency = std::thread::hardware_concurrency();
	for (uint64_t idx = 0; idx < num_hw_concurrency; idx++) {
		auto worker = std::make_shared<datapath::threadpool::pool::worker>(1 << idx);
		this->_workers.insert({idx, worker});
	}
}

datapath::threadpool::pool::~pool()
{
	this->_workers.clear();
}

bool datapath::threadpool::pool::push(std::shared_ptr<task> task)
{
	// Early-Exit tests.
	/// Check for null or invalid tasks.
	if (!task) {
		throw std::invalid_argument("task must not be nullptr");
	}
	if (!task->function) {
		throw std::invalid_argument("task->function must not be nullptr");
	}
	/// Check for invalid affinity masks.
	if ((task->mask & (this->_workers.size() - 1)) == 0) {
		throw std::invalid_argument("mask does not fit any thread");
	}

	affinity_t lowest_id;
	size_t     lowest_count = std::numeric_limits<size_t>::max();
	for (auto kv : _workers) {
		if ((kv.second->affinity & task->mask) == 0) {
			continue;
		}

		std::unique_lock<std::mutex> lock(kv.second->mutex);
		if (kv.second->queue.size() < lowest_count) {
			lowest_id    = kv.first;
			lowest_count = kv.second->queue.size();
		}
	}
	if (lowest_count == std::numeric_limits<size_t>::max()) {
		return false;
	}

	this->_workers[lowest_id]->push(task);
	return true;
}

void datapath::threadpool::pool::clear(affinity_t mask)
{
	// Early-Exit tests.
	if ((mask & (this->_workers.size() - 1)) == 0) {
		throw std::invalid_argument("mask does not fit any thread");
	}

	for (auto kv : _workers) {
		if ((kv.second->affinity & mask) == 0) {
			continue;
		}

		kv.second->clear();
	}
}
