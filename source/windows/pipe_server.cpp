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

#include "pipe_server.hpp"
#include <algorithm>
#include "pipe_connection.hpp"
#include "security.hpp"

datapath::windows::pipe_listener::pipe_listener(std::shared_ptr<iocp_context> ctx, std::wstring path,
												const datapath::options& opts)
	: _ctx(std::move(ctx)), _path(std::move(path)), _opts(opts)
{
	_offer_op.kind = k_offer;
	if (_opts.backlog == 0)
		_opts.backlog = 1;
}

datapath::windows::pipe_listener::~pipe_listener()
{
	for (auto& inst : _pending)
		if (inst->h != INVALID_HANDLE_VALUE)
			CloseHandle(inst->h);
	if (_sd)
		LocalFree(_sd);
}

datapath::error datapath::windows::pipe_listener::create()
{
	_sd = make_security_descriptor(_opts.permissions);
	if (!_sd)
		return datapath::error::CriticalFailure;
	_sa.nLength              = sizeof(_sa);
	_sa.lpSecurityDescriptor = _sd;
	_sa.bInheritHandle       = FALSE;

	// An existing server makes WaitNamedPipe succeed or time out instead of
	// reporting ERROR_FILE_NOT_FOUND. FILE_FLAG_FIRST_PIPE_INSTANCE below is the
	// authoritative check; this one also covers emulators that ignore the flag.
	if (WaitNamedPipeW(_path.c_str(), 1) || GetLastError() == ERROR_SEM_TIMEOUT)
		return datapath::error::AddressInUse;

	std::lock_guard<std::mutex> l(_lock);
	auto                        ec = spawn_locked(true);
	if (ec != datapath::error::Success)
		return ec;
	refill_locked();
	return datapath::error::Success;
}

datapath::error datapath::windows::pipe_listener::spawn_locked(bool first)
{
	DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
	if (first)
		open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
	DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
	DWORD buffer    = DWORD(std::min<size_t>(_opts.receive_buffer, 64u << 20));

	for (int attempt = 0; attempt < 4; attempt++) {
		HANDLE h = CreateNamedPipeW(_path.c_str(), open_mode, pipe_mode, PIPE_UNLIMITED_INSTANCES, buffer, buffer, 0,
									&_sa);
		if (h == INVALID_HANDLE_VALUE) {
			DWORD err = GetLastError();
			if (first && (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY))
				return datapath::error::AddressInUse;
			if (err == ERROR_INVALID_NAME || err == ERROR_FILENAME_EXCED_RANGE)
				return datapath::error::InvalidPath;
			return map_error(err);
		}

		auto inst  = std::make_unique<instance>();
		inst->h    = h;
		inst->skip = _ctx->associate(h);
		inst->op.kind = k_connect;
		inst->op.keep = shared_from_this();
		_ctx->op_started();
		BOOL  ok  = ConnectNamedPipe(h, &inst->op);
		DWORD err = ok ? ERROR_PIPE_CONNECTED : GetLastError();
		if (err == ERROR_IO_PENDING) {
			_pending.push_back(std::move(inst));
			return datapath::error::Success;
		}
		inst->op.keep.reset();
		_ctx->op_finished();
		if (err == ERROR_PIPE_CONNECTED) {
			// A client was already waiting: complete it on an I/O thread.
			instance* raw = inst.get();
			_pending.push_back(std::move(inst));
			if (!_ctx->post(raw->op, shared_from_this())) {
				CloseHandle(raw->h);
				_pending.pop_back();
				return datapath::error::Closed;
			}
			return datapath::error::Success;
		}
		// ERROR_NO_DATA: a client connected and left already; try a fresh instance.
		CloseHandle(h);
		if (err != ERROR_NO_DATA)
			return map_error(err);
		first = false;
		open_mode &= ~DWORD(FILE_FLAG_FIRST_PIPE_INSTANCE);
	}
	return datapath::error::Failure;
}

void datapath::windows::pipe_listener::refill_locked()
{
	while (!_closed && _pending.size() < _opts.backlog
		   && (_opts.max_clients == 0 || _connected + _pending.size() < _opts.max_clients)) {
		if (spawn_locked(false) != datapath::error::Success)
			break;
	}
}

void datapath::windows::pipe_listener::on_io(io_op* op, DWORD, DWORD err)
{
	if (op->kind == k_offer) {
		for (;;) {
			std::shared_ptr<pipe_connection> c;
			{
				std::lock_guard<std::mutex> l(_lock);
				if (_waiting.empty() || !_handler) {
					_offer_posted = false;
					return;
				}
				c = std::move(_waiting.front());
				_waiting.pop_front();
			}
			offer(std::move(c));
		}
	}

	std::shared_ptr<pipe_connection> conn;
	{
		std::lock_guard<std::mutex> l(_lock);
		auto it = std::find_if(_pending.begin(), _pending.end(), [op](auto& i) { return &i->op == op; });
		if (it == _pending.end())
			return;
		std::unique_ptr<instance> inst = std::move(*it);
		_pending.erase(it);
		if (_closed || (err != ERROR_SUCCESS && err != ERROR_PIPE_CONNECTED)) {
			if (inst->h != INVALID_HANDLE_VALUE)
				CloseHandle(inst->h);
			refill_locked();
			return;
		}
		_connected++;
		conn = std::make_shared<pipe_connection>(_ctx, inst->h, inst->skip, _opts, _opts.max_message_size);
		inst->h = INVALID_HANDLE_VALUE;
		_conns.erase(std::remove_if(_conns.begin(), _conns.end(), [](auto& w) { return w.expired(); }), _conns.end());
		_conns.push_back(conn);
		refill_locked();
	}
	_ctx->add_member(std::weak_ptr<datapath::detail::connection>(conn));
	conn->begin_server_handshake(weak_from_this(), _opts.max_message_size);
}

void datapath::windows::pipe_listener::offer(std::shared_ptr<pipe_connection> conn)
{
	datapath::detail::accept_fn handler;
	{
		std::lock_guard<std::mutex> l(_lock);
		if (_closed) {
			conn->close();
			return;
		}
		if (!_handler) {
			_waiting.push_back(std::move(conn));
			return;
		}
		handler = _handler;
	}
	bool                                  accepted = false;
	std::lock_guard<std::recursive_mutex> cb(_callback_lock);
	{
		// release() may have run while we waited for the callback lock.
		std::lock_guard<std::mutex> l(_lock);
		if (_closed) {
			conn->close();
			return;
		}
	}
	try {
		accepted = handler(conn);
	} catch (...) {
		accepted = false;
	}
	if (accepted && conn->good())
		conn->complete_accept();
	else
		conn->close();
}

void datapath::windows::pipe_listener::connection_finished()
{
	std::lock_guard<std::mutex> l(_lock);
	if (_connected)
		_connected--;
	refill_locked();
}

void datapath::windows::pipe_listener::set_accept_handler(datapath::detail::accept_fn handler)
{
	std::lock_guard<std::mutex> l(_lock);
	_handler = std::move(handler);
	if (_handler && !_waiting.empty() && !_offer_posted) {
		_offer_posted = _ctx->post(_offer_op, shared_from_this());
	}
}

void datapath::windows::pipe_listener::release()
{
	close();
	std::lock_guard<std::recursive_mutex> cb(_callback_lock);
}

datapath::error datapath::windows::pipe_listener::close()
{
	std::vector<std::weak_ptr<pipe_connection>>  conns;
	std::deque<std::shared_ptr<pipe_connection>> waiting;
	{
		std::lock_guard<std::mutex> l(_lock);
		if (_closed)
			return datapath::error::Closed;
		_closed = true;
		for (auto& inst : _pending) {
			if (inst->h != INVALID_HANDLE_VALUE) {
				CancelIoEx(inst->h, nullptr);
				CloseHandle(inst->h);
				inst->h = INVALID_HANDLE_VALUE;
			}
		}
		conns.swap(_conns);
		waiting.swap(_waiting);
		_handler = nullptr;
	}
	for (auto& c : waiting)
		c->close();
	for (auto& w : conns)
		if (auto c = w.lock())
			c->close();
	return datapath::error::Success;
}
