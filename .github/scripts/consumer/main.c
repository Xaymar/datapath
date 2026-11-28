/* Low Latency IPC Library for high-speed traffic
 * Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <datapath/datapath.h>
#include <stdio.h>

int main(void)
{
	uint32_t v = datapath_version();
	printf("C: datapath %u.%u.%u, C API %u\n", (unsigned)(v >> 24), (unsigned)((v >> 8) & 0xffff),
		   (unsigned)(v & 0xff), (unsigned)datapath_c_abi_version());
	return datapath_c_abi_version() == DATAPATH_C_ABI_VERSION ? 0 : 1;
}
