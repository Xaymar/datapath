#include <datapath/datapath.h>
#include <datapath/datapath.hpp>
#include <datapath/version.hpp>

#include <cstdio>

int main()
{
	const auto v = datapath::version();
	std::printf("datapath %u.%u.%u (C API %u)\n", unsigned(v.major), unsigned(v.minor), unsigned(v.patch),
				unsigned(datapath_c_abi_version()));
	return v.major == DATAPATH_VERSION_MAJOR ? 0 : 1;
}
