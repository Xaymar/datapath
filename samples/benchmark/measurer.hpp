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

#include <chrono>
#include <map>
#include <memory>
#include <mutex>

class measurer : std::enable_shared_from_this<measurer> {
	std::map<std::chrono::nanoseconds, size_t> timings;

	std::mutex lock;

	public:
	class instance {
		std::shared_ptr<measurer>                      parent;
		std::chrono::high_resolution_clock::time_point start;

		public:
		instance(std::shared_ptr<measurer> parent);

		~instance();

		void cancel();

		void reparent(std::shared_ptr<measurer> parent);
	};

	public:
	measurer();

	~measurer();

	std::shared_ptr<measurer::instance> track();

	void track(std::chrono::nanoseconds duration);

	uint64_t count();

	std::chrono::nanoseconds total_duration();

	double_t average_duration();

	std::chrono::nanoseconds percentile(double_t percentile, bool by_time = false);
};
