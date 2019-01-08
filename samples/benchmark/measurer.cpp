/*
Sample for DataPath
Copyright (C) 2019 Michael Fabian Dirks <info@xaymar.com>

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

#include "measurer.hpp"
#include <iterator>

measurer::instance::instance(std::shared_ptr<measurer> parent)
    : parent(parent), start(std::chrono::high_resolution_clock::now())
{}

measurer::instance::~instance()
{
	auto end = std::chrono::high_resolution_clock::now();
	auto dur = end - this->start;
	if (this->parent) {
		this->parent->track(dur);
	}
}

void measurer::instance::cancel()
{
	this->parent.reset();
}

void measurer::instance::reparent(std::shared_ptr<measurer> parent)
{
	this->parent = parent;
}

measurer::measurer() {}

measurer::~measurer() {}

std::shared_ptr<measurer::instance> measurer::track()
{
	return std::make_shared<measurer::instance>(this->shared_from_this());
}

void measurer::track(std::chrono::nanoseconds duration)
{
	std::unique_lock<std::mutex> ul(this->lock);
	auto                         itr = timings.find(duration);
	if (itr == timings.end()) {
		timings.insert({duration, 1});
	} else {
		itr->second++;
	}
}

uint64_t measurer::count()
{
	uint64_t count = 0;

	std::map<std::chrono::nanoseconds, size_t> copy_timings;
	{
		std::unique_lock<std::mutex> ul(this->lock);
		std::copy(this->timings.begin(), this->timings.end(), std::inserter(copy_timings, copy_timings.end()));
	}

	for (auto kv : copy_timings) {
		count += kv.second;
	}

	return count;
}

std::chrono::nanoseconds measurer::total_duration()
{
	std::chrono::nanoseconds duration;

	std::map<std::chrono::nanoseconds, size_t> copy_timings;
	{
		std::unique_lock<std::mutex> ul(this->lock);
		std::copy(this->timings.begin(), this->timings.end(), std::inserter(copy_timings, copy_timings.end()));
	}

	for (auto kv : copy_timings) {
		duration += kv.first * kv.second;
	}

	return duration;
}

double_t measurer::average_duration()
{
	std::chrono::nanoseconds duration;
	uint64_t                 count = 0;

	std::map<std::chrono::nanoseconds, size_t> copy_timings;
	{
		std::unique_lock<std::mutex> ul(this->lock);
		std::copy(this->timings.begin(), this->timings.end(), std::inserter(copy_timings, copy_timings.end()));
	}

	for (auto kv : copy_timings) {
		duration += kv.first * kv.second;
		count += kv.second;
	}

	return double_t(duration.count()) / double_t(count);
}

template<typename T>
inline bool is_equal(T a, T b, T c)
{
	return (a == b) || ((a >= (b - c)) && (a <= (b + c)));
}

std::chrono::nanoseconds measurer::percentile(double_t percentile, bool by_time)
{
	uint64_t calls = count();

	std::map<std::chrono::nanoseconds, size_t> copy_timings;
	{
		std::unique_lock<std::mutex> ul(this->lock);
		std::copy(this->timings.begin(), this->timings.end(), std::inserter(copy_timings, copy_timings.end()));
	}
	if (by_time) { // Return by time percentile.
		// Find largest and smallest time.
		std::chrono::nanoseconds smallest = copy_timings.begin()->first;
		std::chrono::nanoseconds largest  = copy_timings.rbegin()->first;

		std::chrono::nanoseconds variance = largest - smallest;
		std::chrono::nanoseconds threshold =
		    std::chrono::nanoseconds(smallest.count() + int64_t(variance.count() * percentile));

		for (auto kv : copy_timings) {
			double_t kv_pct = double_t((kv.first - smallest).count()) / double_t(variance.count());
			if (is_equal(kv_pct, percentile, 0.00005) || (kv_pct > percentile)) {
				return std::chrono::nanoseconds(kv.first);
			}
		}
	} else { // Return by call percentile.
		if (percentile == 0.0) {
			return copy_timings.begin()->first;
		}

		uint64_t accu_calls_now = 0;
		for (auto kv : copy_timings) {
			uint64_t accu_calls_last = accu_calls_now;
			accu_calls_now += kv.second;

			double_t percentile_last = double_t(accu_calls_last) / double_t(calls);
			double_t percentile_now  = double_t(accu_calls_now) / double_t(calls);

			if (is_equal(percentile, percentile_now, 0.0005)
			    || ((percentile_last < percentile) && (percentile_now > percentile))) {
				return std::chrono::nanoseconds(kv.first);
			}
		}
	}

	return std::chrono::nanoseconds(-1);
}
