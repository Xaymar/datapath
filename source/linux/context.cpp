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

#include "common/detail.hpp"

namespace {
	class stub_context : public datapath::detail::context_impl {
		public:
		datapath::error connect(std::shared_ptr<datapath::detail::connection>&, const std::string&,
								const datapath::options&) override
		{
			return datapath::error::NotSupported;
		}
		datapath::error listen(std::shared_ptr<datapath::detail::listener>&, const std::string&,
							   const datapath::options&) override
		{
			return datapath::error::NotSupported;
		}
		void shutdown() override {}
	};
} // namespace

std::shared_ptr<datapath::detail::context_impl> datapath::detail::make_platform_context(const datapath::context_options&)
{
	return std::make_shared<stub_context>();
}
