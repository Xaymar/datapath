/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <datapath/export.h>
#include <memory>
#include "options.hpp"

namespace datapath {
	namespace detail {
		class context_impl;
	}

	// Owns the I/O threads. Every socket and server runs on exactly one context.
	// Destroying the last reference closes everything on it and joins its threads.
#ifdef _MSC_VER
// C4251 (std::shared_ptr member without dll-interface) is harmless: a C++ API
// already requires the library and program to share one C++ runtime.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
	class DATAPATH_API context {
		std::shared_ptr<detail::context_impl> _impl;

		public:
		explicit context(std::shared_ptr<detail::context_impl> impl);
		~context();
		context(const context&)            = delete;
		context& operator=(const context&) = delete;

		static std::shared_ptr<context> create(const context_options& options = {});
		// Process-wide context, created on first use with default options.
		static std::shared_ptr<context> get_default();

		const std::shared_ptr<detail::context_impl>& impl() const noexcept
		{
			return _impl;
		}
	};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
} // namespace datapath
