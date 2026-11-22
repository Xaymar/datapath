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

	// Bytes a write can hand to the pipe without waiting, from
	// NtQueryInformationFile (FilePipeLocalInformation): the free outbound buffer
	// space, capped at the buffer's size. WriteQuotaAvailable also counts reads
	// the peer has pending, and a write larger than the buffer itself was seen
	// to wait (1 MiB frames on a 1 MiB pipe). 0 when it can't be read; the
	// buffer's size goes to `outbound`.
	uint64_t write_quota_available(HANDLE h, uint64_t& outbound) noexcept
	{
		struct io_status_block {
			union {
				LONG  Status;
				PVOID Pointer;
			};
			ULONG_PTR Information;
		};
		struct pipe_local_information {
			ULONG NamedPipeType, NamedPipeConfiguration, MaximumInstances, CurrentInstances, InboundQuota,
				ReadDataAvailable, OutboundQuota, WriteQuotaAvailable, NamedPipeState, NamedPipeEnd;
		};
		using query_t  = LONG(NTAPI*)(HANDLE, io_status_block*, PVOID, ULONG, int);
		static auto fn = reinterpret_cast<query_t>(
			reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile")));
		if (!fn)
			return 0;
		io_status_block        iosb{};
		pipe_local_information info{};
		if (fn(h, &iosb, &info, sizeof(info), 24 /*FilePipeLocalInformation*/) < 0)
			return 0;
		outbound = info.OutboundQuota;
		return info.WriteQuotaAvailable < info.OutboundQuota ? info.WriteQuotaAvailable : info.OutboundQuota;
	}
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
	if (_direct_event)
		CloseHandle(_direct_event);
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

bool datapath::windows::pipe_connection::direct_write_wanted(uint64_t frame_size) const noexcept
{
	// Pipes have no gather write, so a direct send writes the header on its own
	// and the reader may wake for it alone. On Irkela that cost more than the
	// saved copy at 64 KiB (p50 12.7 -> 16 us) and less at 256 KiB (71 -> 53 us).
	// A frame larger than the pipe buffer is never sent directly (see
	// platform_write_direct), so skip the attempt: it still cost ~10 us p50 and
	// ~100 us p99 at 1 MiB on Irkela.
	uint64_t outbound = _outbound_quota.load(std::memory_order_relaxed);
	return frame_size > 2 * datapath::detail::send_queue::coalesce_limit && (!outbound || frame_size <= outbound);
}

size_t datapath::windows::pipe_connection::platform_write_direct(const uint8_t* header, size_t header_size,
																const buffer_view* parts, size_t count)
{
	std::shared_lock<std::shared_mutex> lock(_handle_lock);
	if (_h == INVALID_HANDLE_VALUE)
		return 0;
	uint64_t total = header_size;
	for (size_t i = 0; i < count; i++)
		total += parts[i].size;
	// Only when the pipe buffer takes the whole frame now: then every write
	// completes at once and the OS keeps no pointer into the caller's buffers.
	// Otherwise the frame is copied into the queue as before.
	uint64_t outbound = 0;
	uint64_t quota    = write_quota_available(_h, outbound);
	if (outbound)
		_outbound_quota.store(outbound, std::memory_order_relaxed);
	if (quota < total)
		return 0;
	if (!_direct_event) {
		_direct_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!_direct_event)
			return 0;
	}
	size_t sent  = 0;
	auto   write = [&](const uint8_t* p, size_t n) -> bool {
		while (n) {
			OVERLAPPED ov{};
			// Low bit set: the completion is not queued to the port.
			ov.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<uintptr_t>(_direct_event) | 1);
			DWORD got = 0;
			if (!WriteFile(_h, p, DWORD(std::min(n, max_io)), &got, &ov)) {
				DWORD err = GetLastError();
				// Not expected after the quota check; wait so nothing still points
				// into the caller's buffer when send() returns.
				if (err != ERROR_IO_PENDING || !GetOverlappedResult(_h, &ov, &got, TRUE)) {
					if (err == ERROR_IO_PENDING)
						err = GetLastError();
					lock.unlock();
					close_with(map_error(err));
					return false;
				}
			}
			sent += got;
			p += got;
			n -= got;
		}
		return true;
	};
	if (!write(header, header_size))
		return 0;
	for (size_t i = 0; i < count; i++)
		if (!write(static_cast<const uint8_t*>(parts[i].data), parts[i].size))
			return 0;
	return sent;
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
