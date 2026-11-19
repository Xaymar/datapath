/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

struct summary {
	double min_us = 0, p50_us = 0, p99_us = 0, p999_us = 0, max_us = 0;
};

inline summary summarize(std::vector<int64_t> ns)
{
	summary s;
	if (ns.empty())
		return s;
	std::sort(ns.begin(), ns.end());
	auto at = [&](double q) {
		size_t i = size_t(q * double(ns.size() - 1) + 0.5);
		return double(ns[std::min(i, ns.size() - 1)]) / 1000.0;
	};
	s.min_us  = double(ns.front()) / 1000.0;
	s.p50_us  = at(0.50);
	s.p99_us  = at(0.99);
	s.p999_us = at(0.999);
	s.max_us  = double(ns.back()) / 1000.0;
	return s;
}
