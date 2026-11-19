/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#pragma once
#include <memory>
#include "options.hpp"

namespace datapath {
	namespace detail {
		class context_impl;
	}

	// Owns the I/O threads. Every socket and server runs on exactly one context.
	// Destroying the last reference closes everything on it and joins its threads.
	class context {
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
} // namespace datapath
