/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include <mutex>
#include "detail.hpp"

datapath::version_info datapath::version() noexcept
{
	return {DATAPATH_VERSION_ABI, DATAPATH_VERSION_PATCH};
}

const char* datapath::to_string(datapath::error e) noexcept
{
	switch (e) {
	case error::Unknown:
		return "Unknown";
	case error::Success:
		return "Success";
	case error::Failure:
		return "Failure";
	case error::CriticalFailure:
		return "CriticalFailure";
	case error::Closed:
		return "Closed";
	case error::TimedOut:
		return "TimedOut";
	case error::InvalidPath:
		return "InvalidPath";
	case error::NotSupported:
		return "NotSupported";
	case error::TooLarge:
		return "TooLarge";
	case error::WouldBlock:
		return "WouldBlock";
	case error::VersionMismatch:
		return "VersionMismatch";
	case error::NotFound:
		return "NotFound";
	case error::AddressInUse:
		return "AddressInUse";
	case error::AccessDenied:
		return "AccessDenied";
	}
	return "Invalid";
}

// context ----------------------------------------------------------------------

datapath::context::context(std::shared_ptr<detail::context_impl> impl) : _impl(std::move(impl)) {}

datapath::context::~context()
{
	if (_impl)
		_impl->shutdown();
}

std::shared_ptr<datapath::context> datapath::context::create(const context_options& options)
{
	auto impl = detail::make_platform_context(options);
	if (!impl)
		return nullptr;
	return std::make_shared<context>(std::move(impl));
}

std::shared_ptr<datapath::context> datapath::context::get_default()
{
	static std::shared_ptr<context> instance = create();
	return instance;
}

// socket -----------------------------------------------------------------------

datapath::socket::socket(std::shared_ptr<detail::connection> impl) : _impl(std::move(impl)) {}

datapath::socket::~socket()
{
	if (_impl)
		_impl->release();
}

void datapath::socket::on_message(message_handler handler)
{
	_impl->set_message_handler(std::move(handler));
}

void datapath::socket::on_close(close_handler handler)
{
	_impl->set_close_handler(std::move(handler));
}

datapath::error datapath::socket::send(const void* data, size_t size)
{
	buffer_view part{data, size};
	return _impl->send(&part, 1);
}

datapath::error datapath::socket::sendv(const buffer_view* parts, size_t count)
{
	return _impl->send(parts, count);
}

datapath::error datapath::socket::close()
{
	return _impl->close();
}

bool datapath::socket::good() const
{
	return _impl->good();
}

uint64_t datapath::socket::max_message_size() const
{
	return _impl->max_message_size();
}

// server -----------------------------------------------------------------------

datapath::server::server(std::shared_ptr<detail::listener> impl) : _impl(std::move(impl)) {}

datapath::server::~server()
{
	if (_impl)
		_impl->release();
}

void datapath::server::on_accept(accept_handler handler)
{
	if (!handler) {
		_impl->set_accept_handler(nullptr);
		return;
	}
	_impl->set_accept_handler([handler](std::shared_ptr<detail::connection> conn) {
		return handler(std::make_shared<datapath::socket>(std::move(conn)));
	});
}

datapath::error datapath::server::close()
{
	return _impl->close();
}

// free functions ---------------------------------------------------------------

datapath::error datapath::connect(std::shared_ptr<datapath::socket>& socket, const std::string& path,
								  const datapath::options& options, std::shared_ptr<datapath::context> ctx)
{
	if (!ctx)
		ctx = context::get_default();
	if (!ctx)
		return error::CriticalFailure;
	std::shared_ptr<detail::connection> conn;
	auto                                ec = ctx->impl()->connect(conn, path, options);
	if (ec != error::Success)
		return ec;
	socket = std::make_shared<datapath::socket>(std::move(conn));
	return error::Success;
}

datapath::error datapath::host(std::shared_ptr<datapath::server>& server, const std::string& path,
							   const datapath::options& options, std::shared_ptr<datapath::context> ctx)
{
	if (!ctx)
		ctx = context::get_default();
	if (!ctx)
		return error::CriticalFailure;
	std::shared_ptr<detail::listener> lst;
	auto                              ec = ctx->impl()->listen(lst, path, options);
	if (ec != error::Success)
		return ec;
	server = std::make_shared<datapath::server>(std::move(lst));
	return error::Success;
}
