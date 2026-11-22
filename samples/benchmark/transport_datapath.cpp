/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include <datapath/datapath.hpp>
#include "bench.hpp"

namespace {
	bench::status map(datapath::error e)
	{
		switch (e) {
		case datapath::error::Success:
			return bench::status::ok;
		case datapath::error::WouldBlock:
			return bench::status::would_block;
		case datapath::error::Closed:
			return bench::status::closed;
		case datapath::error::NotFound:
			return bench::status::not_found;
		default:
			return bench::status::failed;
		}
	}

	class dp_connection : public bench::connection {
		std::shared_ptr<datapath::socket> _s;

		public:
		explicit dp_connection(std::shared_ptr<datapath::socket> s) : _s(std::move(s)) {}
		bench::status send(const void* data, size_t size) override
		{
			return map(_s->send(data, size));
		}
		void on_message(bench::message_fn fn) override
		{
			_s->on_message(std::move(fn));
		}
	};

	class dp_transport : public bench::transport {
		std::shared_ptr<datapath::context> _ctx;
		datapath::options                  _opts;
		std::shared_ptr<datapath::server>  _server;

		public:
		explicit dp_transport(const bench::config& c)
		{
			datapath::context_options o;
			o.spin       = std::chrono::microseconds(c.spin);
			o.io_threads = c.threads;
			o.yield      = c.yield;
			o.alertable  = c.alertable;
			o.pair_peer  = c.pair;
			o.mmcss      = c.mmcss;
			o.priority   = c.priority;
			o.ideal_core = c.ideal_core;
			_ctx         = datapath::context::create(o);

			_opts.max_message_size = uint64_t(1) << 30;
			_opts.max_queued_bytes = uint64_t(256) << 20;
			_opts.connect_timeout  = std::chrono::milliseconds(5000);
			_opts.receive_buffer   = c.buffer;
			_opts.pipe_buffer      = c.pipe_buffer;
		}

		const char* name() const override
		{
			return "datapath";
		}

		bench::status host(const std::string& path, std::function<void(bench::conn_ptr)> on_accept) override
		{
			auto ec = datapath::host(_server, path, _opts, _ctx);
			if (ec != datapath::error::Success)
				return map(ec);
			_server->on_accept([on_accept](std::shared_ptr<datapath::socket> s) {
				on_accept(std::make_shared<dp_connection>(std::move(s)));
				return true;
			});
			return bench::status::ok;
		}

		bench::status connect(const std::string& path, bench::conn_ptr& out) override
		{
			std::shared_ptr<datapath::socket> s;
			auto                              ec = datapath::connect(s, path, _opts, _ctx);
			if (ec != datapath::error::Success)
				return map(ec);
			out = std::make_shared<dp_connection>(std::move(s));
			return bench::status::ok;
		}
	};
} // namespace

std::unique_ptr<bench::transport> bench::make_transport(const config& c)
{
	return std::make_unique<dp_transport>(c);
}
