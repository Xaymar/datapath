/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "pipe_connection.hpp"
#include <algorithm>
#include "pipe_server.hpp"

namespace {
	constexpr size_t max_io = size_t(1) << 30;
}

datapath::windows::pipe_connection::pipe_connection(std::shared_ptr<iocp_context> ctx, HANDLE h,
													bool skip_on_success, const datapath::options& opts,
													uint64_t negotiated_limit)
	: connection_base(opts, negotiated_limit), _ctx(std::move(ctx)), _h(h), _skip_on_success(skip_on_success)
{
	_rop.kind = k_read;
	_wop.kind = k_write;
	_aop.kind = k_arm;
	_cop.kind = k_close;
	_hop.kind = k_hello;
}

datapath::windows::pipe_connection::~pipe_connection()
{
	if (_h != INVALID_HANDLE_VALUE)
		CloseHandle(_h);
}

datapath::windows::pipe_connection::io_result datapath::windows::pipe_connection::issue(bool write, const uint8_t* buf,
																					   size_t size, io_op& op,
																					   DWORD& bytes, DWORD& err)
{
	std::shared_lock<std::shared_mutex> lock(_handle_lock);
	if (_h == INVALID_HANDLE_VALUE) {
		err = ERROR_INVALID_HANDLE;
		return io_result::failed;
	}
	DWORD n = DWORD(std::min(size, max_io));
	op.reset();
	op.keep = self();
	_ctx->op_started();
	bytes  = 0;
	BOOL ok = write ? WriteFile(_h, buf, n, &bytes, &op) : ReadFile(_h, const_cast<uint8_t*>(buf), n, &bytes, &op);
	if (ok) {
		if (!_skip_on_success)
			return io_result::pending; // a completion packet follows anyway
		op.keep.reset();
		_ctx->op_finished();
		return io_result::done;
	}
	err = GetLastError();
	if (err == ERROR_IO_PENDING)
		return io_result::pending;
	op.keep.reset();
	_ctx->op_finished();
	return io_result::failed;
}

void datapath::windows::pipe_connection::read_loop()
{
	for (;;) {
		if (_closed.load(std::memory_order_acquire)) {
			finish_close();
			return;
		}
		auto  area  = _parser.prepare();
		DWORD bytes = 0, err = 0;
		switch (issue(false, area.first, area.second, _rop, bytes, err)) {
		case io_result::done:
			if (!consume(bytes))
				return;
			continue;
		case io_result::pending:
			return;
		case io_result::failed:
			close_with(map_error(err));
			finish_close();
			return;
		}
	}
}

void datapath::windows::pipe_connection::write_loop()
{
	for (;;) {
		if (_woff >= _wbatch.size()) {
			_woff = 0;
			if (!_queue.take(_wbatch)) {
				if (_close_when_flushed)
					close_with(datapath::error::VersionMismatch);
				return;
			}
		}
		DWORD bytes = 0, err = 0;
		switch (issue(true, _wbatch.data() + _woff, _wbatch.size() - _woff, _wop, bytes, err)) {
		case io_result::done:
			_woff += bytes;
			continue;
		case io_result::pending:
			return;
		case io_result::failed:
			close_with(map_error(err));
			return;
		}
	}
}

void datapath::windows::pipe_connection::hello_loop()
{
	for (;;) {
		if (_closed.load(std::memory_order_acquire))
			return;
		DWORD bytes = 0, err = 0;
		switch (issue(false, _hello + _hello_got, sizeof(_hello) - _hello_got, _hop, bytes, err)) {
		case io_result::done:
			_hello_got += bytes;
			if (_hello_got == sizeof(_hello)) {
				hello_done();
				return;
			}
			continue;
		case io_result::pending:
			return;
		case io_result::failed:
			close_with(map_error(err));
			return;
		}
	}
}

void datapath::windows::pipe_connection::hello_done()
{
	datapath::detail::hello h{};
	if (datapath::detail::decode_hello(_hello, h) != datapath::error::Success) {
		// Answer with our own hello so the client can report VersionMismatch,
		// then close once it is written.
		_graceful           = true;
		_close_when_flushed = true;
		complete_accept();
		return;
	}
	uint64_t limit = std::min(_own_limit, h.max_message_size);
	_parser.set_limit(limit);
	_queue.set_limit(limit);
	if (auto l = _listener.lock()) {
		l->offer(self());
	} else {
		close_with(datapath::error::Closed);
	}
}

void datapath::windows::pipe_connection::begin_server_handshake(std::weak_ptr<pipe_listener> listener,
																uint64_t                     own_limit)
{
	_listener    = std::move(listener);
	_own_limit   = own_limit;
	_server_side = true;
	_queue.try_acquire(); // replies must go out before anything the user queues
	hello_loop();
}

void datapath::windows::pipe_connection::complete_accept()
{
	_wbatch.resize(datapath::detail::hello_size);
	datapath::detail::encode_hello(_wbatch.data(), {datapath::detail::protocol_version, _own_limit});
	_woff = 0;
	write_loop();
}

void datapath::windows::pipe_connection::platform_close()
{
	std::unique_lock<std::shared_mutex> lock(_handle_lock);
	if (_h != INVALID_HANDLE_VALUE) {
		// Disconnect the server end so the pipe name is released even while the
		// client still holds its handle (otherwise re-hosting fails). Skipped for
		// a graceful close that must let the client read what was written.
		if (_server_side && !_graceful)
			DisconnectNamedPipe(_h);
		CancelIoEx(_h, nullptr);
		CloseHandle(_h);
		_h = INVALID_HANDLE_VALUE;
	}
}

void datapath::windows::pipe_connection::platform_arm()
{
	// Hop onto an I/O thread so messages are never delivered on the caller's thread.
	if (!_ctx->post(_aop, self()))
		close_with(datapath::error::Closed), finish_close();
}

void datapath::windows::pipe_connection::platform_post_close()
{
	if (!_ctx->post(_cop, self()))
		finish_close();
}

void datapath::windows::pipe_connection::platform_write()
{
	write_loop();
}

void datapath::windows::pipe_connection::after_inline_write()
{
	// Let a receiver that our write just woke run on this core right away.
	if (_ctx->yield_enabled())
		Sleep(0);
}

void datapath::windows::pipe_connection::on_finished()
{
	if (auto l = _listener.lock())
		l->connection_finished();
}

void datapath::windows::pipe_connection::on_io(io_op* op, DWORD bytes, DWORD err)
{
	switch (op->kind) {
	case k_read:
		if (err != ERROR_SUCCESS) {
			close_with(map_error(err));
			finish_close();
			return;
		}
		if (consume(bytes))
			read_loop();
		return;
	case k_write:
		if (err != ERROR_SUCCESS) {
			close_with(map_error(err));
			// Keep the writer slot: the queue is closed, nothing more goes out.
			return;
		}
		_woff += bytes;
		write_loop();
		return;
	case k_arm:
		read_loop();
		return;
	case k_close:
		finish_close();
		return;
	case k_hello:
		if (err != ERROR_SUCCESS) {
			close_with(map_error(err));
			return;
		}
		_hello_got += bytes;
		if (_hello_got == sizeof(_hello))
			hello_done();
		else
			hello_loop();
		return;
	}
}
