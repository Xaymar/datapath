// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause
#include <datapath/datapath.hpp>
#include <datapath/version.hpp>
#include <cstdio>

int main()
{
	const auto v = datapath::version();
	std::printf("C++: datapath %u.%u.%u\n", unsigned(v.major), unsigned(v.minor), unsigned(v.patch));
	return DATAPATH_MAKE_VERSION(v.major, v.minor, v.patch) == DATAPATH_VERSION ? 0 : 1;
}
