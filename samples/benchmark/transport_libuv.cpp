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

// libuv transport for the comparison benchmark.
//
// Written the way libuv is normally used: one event-loop thread per process;
// sends from other threads are marshalled onto it with uv_async, sends from
// inside callbacks write directly. For a fair comparison it uses DataPath's
// frame parser, the same spin-then-block policy (--spin), and the same thread
// scheduling (--priority, --mmcss) on its loop thread.

#include <uv.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "bench.hpp"
#include "common/framing.hpp"
#include "common/spin.hpp"

namespace {
	class uv_transport;

	class uv_conn : public bench::connection {
		public:
		static constexpr uint64_t max_queued = uint64_t(256) << 20;

		uv_transport*                  tp;
		uv_pipe_t                      pipe{};
		datapath::detail::frame_parser parser;
		std::mutex                     handler_lock;
		bench::message_fn              handler;
		bool                           reading = false; // loop thread only
		std::atomic<bool>              closed{false};
		std::atomic<uint64_t>          queued{0};

		uv_conn(uv_transport* t, size_t buffer) : tp(t), parser(uint64_t(1) << 30, buffer)
		{
			pipe.data = this;
		}

		bench::status send(const void* data, size_t size) override;
		void          on_message(bench::message_fn fn) override;

		void start_read(); // loop thread
		void write_now(std::vector<uint8_t>&& frame);
	};

	struct write_req {
		uv_write_t           req;
		std::vector<uint8_t> data;
		uv_conn*             conn;
	};

	class uv_transport : public bench::transport {
		public:
		bench::config cfg;
		uv_loop_t     loop{};
		uv_async_t    async{};
		std::thread   thread;
		std::atomic<std::thread::id> loop_id{};
		std::mutex                   task_lock;
		std::vector<std::function<void()>> tasks;
		std::atomic<bool>                  stop{false};
		uint64_t                           activity = 0; // loop thread only

		std::vector<std::shared_ptr<uv_conn>> conns; // loop thread; live until exit
		uv_pipe_t                             server{};
		std::function<void(bench::conn_ptr)>  accept_fn;

		explicit uv_transport(const bench::config& c) : cfg(c)
		{
			uv_loop_init(&loop);
			async.data = this;
			uv_async_init(&loop, &async, [](uv_async_t* a) {
				auto*                              self = static_cast<uv_transport*>(a->data);
				std::vector<std::function<void()>> run;
				{
					std::lock_guard<std::mutex> l(self->task_lock);
					run.swap(self->tasks);
				}
				for (auto& t : run)
					t();
				self->activity++;
			});
			thread = std::thread([this] {
				loop_id = std::this_thread::get_id();
				bench::tune_io_thread(cfg);
				auto budget = std::chrono::microseconds(cfg.spin);
				while (!stop.load()) {
					datapath::detail::spin_then_block(
						budget,
						[&] {
							uint64_t before = activity;
							uv_run(&loop, UV_RUN_NOWAIT);
							return activity != before;
						},
						[&] {
							if (!stop.load())
								uv_run(&loop, UV_RUN_ONCE);
						});
				}
			});
		}

		~uv_transport() override
		{
			stop = true;
			uv_async_send(&async);
			if (thread.joinable())
				thread.join();
		}

		bool on_loop() const
		{
			return std::this_thread::get_id() == loop_id.load();
		}

		void post(std::function<void()> fn)
		{
			{
				std::lock_guard<std::mutex> l(task_lock);
				tasks.push_back(std::move(fn));
			}
			uv_async_send(&async);
		}

		template<typename T, typename F>
		T call(F&& fn)
		{
			auto promise = std::make_shared<std::promise<T>>();
			auto future  = promise->get_future();
			post([promise, fn = std::forward<F>(fn)]() mutable { fn(promise); });
			return future.get();
		}

		static std::string pipe_path(const std::string& name)
		{
#ifdef _WIN32
			return "\\\\.\\pipe\\uv." + name;
#else
			return std::string("\0uv.", 4) + name; // abstract namespace
#endif
		}

		const char* name() const override
		{
			static const std::string n = std::string("libuv ") + uv_version_string();
			return n.c_str();
		}

		bench::status host(const std::string& path, std::function<void(bench::conn_ptr)> on_accept) override
		{
			accept_fn = std::move(on_accept);
			return call<bench::status>([this, path](std::shared_ptr<std::promise<bench::status>> p) {
				std::string full = pipe_path(path);
				uv_pipe_init(&loop, &server, 0);
				server.data = this;
				if (uv_pipe_bind2(&server, full.data(), full.size(), 0) != 0) {
					p->set_value(bench::status::failed);
					return;
				}
				int rc = uv_listen(reinterpret_cast<uv_stream_t*>(&server), 128, [](uv_stream_t* s, int status) {
					auto* self = static_cast<uv_transport*>(s->data);
					if (status < 0)
						return;
					auto c = std::make_shared<uv_conn>(self, self->cfg.buffer);
					uv_pipe_init(&self->loop, &c->pipe, 0);
					if (uv_accept(s, reinterpret_cast<uv_stream_t*>(&c->pipe)) != 0)
						return;
					self->conns.push_back(c);
					self->accept_fn(c);
					self->activity++;
				});
				p->set_value(rc == 0 ? bench::status::ok : bench::status::failed);
			});
		}

		bench::status connect(const std::string& path, bench::conn_ptr& out) override
		{
			struct state {
				uv_connect_t                                    req;
				std::shared_ptr<uv_conn>                        conn;
				std::shared_ptr<std::promise<bench::status>> promise;
			};
			auto conn = std::make_shared<uv_conn>(this, cfg.buffer);
			auto st   = call<bench::status>([this, conn, path](std::shared_ptr<std::promise<bench::status>> p) {
                std::string full = pipe_path(path);
                uv_pipe_init(&loop, &conn->pipe, 0);
                auto* s     = new state{{}, conn, p};
                s->req.data = s;
                int rc      = uv_pipe_connect2(&s->req, &conn->pipe, full.data(), full.size(), 0,
                                               [](uv_connect_t* req, int status) {
                                                   auto* s = static_cast<state*>(req->data);
                                                   auto* tp = s->conn->tp;
                                                   if (status == 0) {
                                                       tp->conns.push_back(s->conn);
                                                       s->promise->set_value(bench::status::ok);
                                                   } else {
                                                       s->promise->set_value(status == UV_ENOENT || status == UV_ECONNREFUSED
                                                                                 ? bench::status::not_found
                                                                                 : bench::status::failed);
                                                       uv_close(reinterpret_cast<uv_handle_t*>(&s->conn->pipe),
                                                                nullptr);
                                                   }
                                                   delete s;
                                               });
                if (rc != 0) {
                    p->set_value(bench::status::failed);
                    delete s;
                }
			});
			if (st == bench::status::ok)
				out = conn;
			return st;
		}
	};

	bench::status uv_conn::send(const void* data, size_t size)
	{
		if (closed.load())
			return bench::status::closed;
		uint64_t frame = size + datapath::detail::frame_header_size;
		uint64_t q     = queued.load();
		if (q > 0 && q + frame > max_queued)
			return bench::status::would_block;
		std::vector<uint8_t> buf(datapath::detail::frame_header_size);
		buf.reserve(size_t(frame));
		datapath::detail::encode_frame_header(buf.data(), size);
		auto* p = static_cast<const uint8_t*>(data);
		buf.insert(buf.end(), p, p + size);
		queued += frame;
		if (tp->on_loop())
			write_now(std::move(buf));
		else
			tp->post([this, b = std::move(buf)]() mutable { write_now(std::move(b)); });
		return bench::status::ok;
	}

	void uv_conn::write_now(std::vector<uint8_t>&& frame)
	{
		auto* w = new write_req{{}, std::move(frame), this};
		uv_buf_t b = uv_buf_init(reinterpret_cast<char*>(w->data.data()), unsigned(w->data.size()));
		int      rc = uv_write(&w->req, reinterpret_cast<uv_stream_t*>(&pipe), &b, 1, [](uv_write_t* r, int status) {
            auto* w = reinterpret_cast<write_req*>(r);
            w->conn->queued -= w->data.size();
            w->conn->tp->activity++;
            if (status < 0)
                w->conn->closed = true;
            delete w;
		});
		if (rc != 0) {
			queued -= w->data.size();
			closed = true;
			delete w;
		}
	}

	void uv_conn::on_message(bench::message_fn fn)
	{
		{
			std::lock_guard<std::mutex> l(handler_lock);
			handler = std::move(fn);
		}
		if (tp->on_loop())
			start_read();
		else
			tp->post([this] { start_read(); });
	}

	void uv_conn::start_read()
	{
		if (reading)
			return;
		reading = true;
		uv_read_start(
			reinterpret_cast<uv_stream_t*>(&pipe),
			[](uv_handle_t* h, size_t, uv_buf_t* buf) {
				auto* c    = static_cast<uv_conn*>(h->data);
				auto  area = c->parser.prepare();
				buf->base  = reinterpret_cast<char*>(area.first);
				buf->len   = decltype(buf->len)(area.second);
			},
			[](uv_stream_t* s, ssize_t nread, const uv_buf_t*) {
				auto* c = static_cast<uv_conn*>(s->data);
				c->tp->activity++;
				if (nread < 0) {
					c->closed = true;
					uv_read_stop(s);
					return;
				}
				if (nread == 0)
					return;
				c->parser.commit(size_t(nread), [c](const uint8_t* d, size_t n) {
					bench::message_fn* h;
					{
						std::lock_guard<std::mutex> l(c->handler_lock);
						h = &c->handler;
					}
					if (*h)
						(*h)(d, n);
				});
			});
	}
} // namespace

std::unique_ptr<bench::transport> bench::make_transport(const config& c)
{
	return std::make_unique<uv_transport>(c);
}
