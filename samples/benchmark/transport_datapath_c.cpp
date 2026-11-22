/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// The benchmark transport over DataPath's C API (datapath.h). It measures the
// same library as transport_datapath.cpp, but through the interface every
// non-C++ binding uses, so a core that only exports the C ABI can be measured
// with this exact harness, clock and framing.

#include <datapath/datapath.h>
#include <utility>
#include "bench.hpp"

namespace {
	bench::status map(datapath_error e)
	{
		switch (e) {
		case DATAPATH_SUCCESS:
			return bench::status::ok;
		case DATAPATH_ERROR_WOULD_BLOCK:
			return bench::status::would_block;
		case DATAPATH_ERROR_CLOSED:
			return bench::status::closed;
		case DATAPATH_ERROR_NOT_FOUND:
			return bench::status::not_found;
		default:
			return bench::status::failed;
		}
	}

	class c_connection : public bench::connection {
		datapath_socket* _s;

		static void deliver(void* user, datapath_socket*, const uint8_t* data, size_t size)
		{
			(*static_cast<bench::message_fn*>(user))(data, size);
		}
		static void free_fn(void* user)
		{
			delete static_cast<bench::message_fn*>(user);
		}

		public:
		explicit c_connection(datapath_socket* s) : _s(s) {}
		~c_connection() override
		{
			datapath_socket_release(_s);
		}
		c_connection(const c_connection&)            = delete;
		c_connection& operator=(const c_connection&) = delete;

		bench::status send(const void* data, size_t size) override
		{
			return map(datapath_socket_send(_s, data, size));
		}
		void on_message(bench::message_fn fn) override
		{
			// The library owns `user` from here on and frees it even on failure.
			datapath_socket_set_on_message(_s, &deliver, new bench::message_fn(std::move(fn)), &free_fn);
		}
	};

	using accept_fn = std::function<void(bench::conn_ptr)>;

	class c_transport : public bench::transport {
		datapath_context* _ctx    = nullptr;
		datapath_server*  _server = nullptr;
		datapath_options  _opts;

		static int accept(void* user, datapath_server*, datapath_socket* client)
		{
			(*static_cast<accept_fn*>(user))(std::make_shared<c_connection>(client));
			return 1;
		}
		static void free_accept(void* user)
		{
			delete static_cast<accept_fn*>(user);
		}

		public:
		explicit c_transport(const bench::config& c)
		{
			datapath_context_options o;
			datapath_context_options_init(&o);
			o.spin_us    = c.spin;
			o.io_threads = c.threads;
			o.yield      = c.yield ? 1 : 0;
			o.alertable  = c.alertable ? 1 : 0;
			o.pair_peer  = c.pair ? 1 : 0;
			o.mmcss      = c.mmcss ? 1 : 0;
			o.priority   = c.priority;
			o.ideal_core = c.ideal_core;
			datapath_context_create(&o, &_ctx);

			datapath_options_init(&_opts);
			_opts.max_message_size   = uint64_t(1) << 30;
			_opts.max_queued_bytes   = uint64_t(256) << 20;
			_opts.connect_timeout_ms = 5000;
			_opts.receive_buffer     = c.buffer;
			_opts.pipe_buffer        = c.pipe_buffer;
		}
		~c_transport() override
		{
			datapath_server_release(_server);
			datapath_context_release(_ctx);
		}
		c_transport(const c_transport&)            = delete;
		c_transport& operator=(const c_transport&) = delete;

		const char* name() const override
		{
			return "datapath-c";
		}

		bench::status host(const std::string& path, accept_fn on_accept) override
		{
			if (!_ctx)
				return bench::status::failed;
			auto ec = datapath_host(path.c_str(), &_opts, _ctx, &_server);
			if (ec != DATAPATH_SUCCESS)
				return map(ec);
			// The library owns the user data from here on and frees it even on failure.
			return map(
				datapath_server_set_on_accept(_server, &accept, new accept_fn(std::move(on_accept)), &free_accept));
		}

		bench::status connect(const std::string& path, bench::conn_ptr& out) override
		{
			if (!_ctx)
				return bench::status::failed;
			datapath_socket* s  = nullptr;
			auto             ec = datapath_connect(path.c_str(), &_opts, _ctx, &s);
			if (ec != DATAPATH_SUCCESS)
				return map(ec);
			out = std::make_shared<c_connection>(s);
			return bench::status::ok;
		}
	};
} // namespace

std::unique_ptr<bench::transport> bench::make_transport(const config& c)
{
	return std::make_unique<c_transport>(c);
}
