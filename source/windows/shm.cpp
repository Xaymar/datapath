/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "shm.hpp"
#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include "common/endian.hpp"
#include "security.hpp"
#include <aclapi.h>
#include <sddl.h>

namespace {
	using namespace datapath::detail::shm;

	const wchar_t* const suffixes[5] = {L"-m", L"-d0", L"-s0", L"-d1", L"-s1"};

	template<typename T>
	T load_proc(const wchar_t* module, const char* name)
	{
		HMODULE m = GetModuleHandleW(module);
		if (!m)
			m = LoadLibraryW(module);
		return m ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
	}

	using create_packet_t    = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID);
	using associate_packet_t = LONG(NTAPI*)(HANDLE, HANDLE, HANDLE, PVOID, PVOID, LONG, ULONG_PTR, PBOOLEAN);
	using valloc2_t          = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
	using map3_t             = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
	using gen_random_t       = BOOLEAN(WINAPI*)(PVOID, ULONG);

	struct nt_api {
		create_packet_t    create_packet    = load_proc<create_packet_t>(L"ntdll.dll", "NtCreateWaitCompletionPacket");
		associate_packet_t associate_packet = load_proc<associate_packet_t>(L"ntdll.dll",
																			  "NtAssociateWaitCompletionPacket");
		valloc2_t          valloc2          = load_proc<valloc2_t>(L"kernelbase.dll", "VirtualAlloc2");
		map3_t             map3             = load_proc<map3_t>(L"kernelbase.dll", "MapViewOfFile3");
		gen_random_t       gen_random       = load_proc<gen_random_t>(L"advapi32.dll", "SystemFunction036");
	};

	const nt_api& api()
	{
		static const nt_api a;
		return a;
	}

	// One ring mapped twice, back to back, so that a record running past the end
	// lands at its start. Placeholders (Windows 10 1803+) make it race-free.
	struct mirror_view {
		uint8_t* base    = nullptr;
		void*    view[2] = {};

		bool map(HANDLE section, uint64_t offset, uint64_t cap)
		{
			constexpr ULONG reserve_placeholder = 0x00040000, replace_placeholder = 0x00004000,
							preserve_placeholder = 0x00000002;
			if (!api().valloc2 || !api().map3)
				return false;
			base = static_cast<uint8_t*>(api().valloc2(nullptr, nullptr, SIZE_T(2 * cap),
													   MEM_RESERVE | reserve_placeholder, PAGE_NOACCESS, nullptr, 0));
			if (!base)
				return false;
			if (!VirtualFree(base, SIZE_T(cap), MEM_RELEASE | preserve_placeholder)) {
				VirtualFree(base, 0, MEM_RELEASE);
				base = nullptr;
				return false;
			}
			for (int i = 0; i < 2; i++) {
				view[i] = api().map3(section, GetCurrentProcess(), base + i * cap, offset, SIZE_T(cap),
									 replace_placeholder, PAGE_READWRITE, nullptr, 0);
				if (!view[i]) {
					release(cap);
					return false;
				}
			}
			return true;
		}

		void release(uint64_t cap) noexcept
		{
			for (int i = 0; i < 2; i++) {
				if (view[i])
					UnmapViewOfFile(view[i]);
				else if (base)
					VirtualFree(base + i * cap, 0, MEM_RELEASE); // still a placeholder
				view[i] = nullptr;
			}
			base = nullptr;
		}
	};

	std::wstring sid_string(PSID sid)
	{
		LPWSTR       str = nullptr;
		std::wstring out;
		if (ConvertSidToStringSidW(sid, &str)) {
			out = str;
			LocalFree(str);
		}
		return out;
	}

	// The pipe client's user SID, read at identification level.
	std::wstring pipe_client_sid(HANDLE pipe)
	{
		if (!ImpersonateNamedPipeClient(pipe))
			return {};
		HANDLE       token = nullptr;
		std::wstring out;
		if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
			DWORD size = 0;
			GetTokenInformation(token, TokenUser, nullptr, 0, &size);
			std::vector<uint8_t> buf(size ? size : 1);
			if (GetTokenInformation(token, TokenUser, buf.data(), size, &size))
				out = sid_string(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid);
			CloseHandle(token);
		}
		RevertToSelf();
		return out;
	}

	// The user SID of the process at the server end of `pipe`, or empty if it
	// can't be read (then the peer counts as another user).
	std::wstring pipe_server_sid(HANDLE pipe)
	{
		ULONG pid = 0;
		if (!GetNamedPipeServerProcessId(pipe, &pid))
			return {};
		HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!proc)
			return {};
		HANDLE       token = nullptr;
		std::wstring out;
		if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
			DWORD size = 0;
			GetTokenInformation(token, TokenUser, nullptr, 0, &size);
			std::vector<uint8_t> buf(size ? size : 1);
			if (GetTokenInformation(token, TokenUser, buf.data(), size, &size))
				out = sid_string(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid);
			CloseHandle(token);
		}
		CloseHandle(proc);
		return out;
	}

	bool is_hex_name(const uint8_t* p, size_t n)
	{
		for (size_t i = 0; i < n; i++)
			if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
				return false;
		return true;
	}
} // namespace

struct datapath::windows::shm_channel::objects {
	HANDLE      section   = nullptr;
	HANDLE      events[4] = {}; // data[0], space[0], data[1], space[1]
	HANDLE      packets[2] = {}; // data, space
	void*       view       = nullptr;
	uint64_t    cap        = 0;
	bool        mirrored   = false;
	bool        other_user = true;
	bool        zero_copy  = false;
	mirror_view mirror[2];

	~objects()
	{
		for (auto& m : mirror)
			m.release(cap);
		if (view)
			UnmapViewOfFile(view);
		if (section)
			CloseHandle(section);
		for (HANDLE e : events)
			if (e)
				CloseHandle(e);
		for (HANDLE p : packets)
			if (p)
				CloseHandle(p);
	}

	bool map_mirrors()
	{
		for (int r = 0; r < 2; r++)
			if (!mirror[r].map(section, data_offset + r * cap, cap))
				return false;
		mirrored = true;
		return true;
	}

	bool make_packets()
	{
		for (auto& p : packets)
			if (api().create_packet(&p, GENERIC_ALL, nullptr) < 0) {
				p = nullptr;
				return false;
			}
		return true;
	}

	section_header* header() const noexcept
	{
		return static_cast<section_header*>(view);
	}
};

bool datapath::windows::shm_channel::supported() noexcept
{
	return api().create_packet && api().associate_packet && api().gen_random;
}

bool datapath::windows::shm_arm_packet(HANDLE packet, HANDLE port, HANDLE event, OVERLAPPED* ov) noexcept
{
	BOOLEAN already = FALSE;
	// Queues `ov` to the port once `event` is signalled (at once if it is).
	return api().associate_packet && api().associate_packet(packet, port, event, nullptr, ov, 0, 0, &already) >= 0;
}

datapath::windows::shm_channel::shm_channel(std::unique_ptr<objects> o, bool server, uint64_t limit,
											uint64_t max_queued)
	: _o(std::move(o))
{
	ring_pair::setup s{};
	s.header   = _o->header();
	s.cap      = _o->cap;
	s.mirrored = _o->mirrored;
	s.other_user = _o->other_user;
	s.zero_copy  = _o->zero_copy;
	s.server   = server;
	if (s.mirrored) {
		s.data[0] = _o->mirror[0].base;
		s.data[1] = _o->mirror[1].base;
	} else {
		s.data[0] = static_cast<uint8_t*>(_o->view) + data_offset;
		s.data[1] = s.data[0] + s.cap;
	}
	s.limit      = limit;
	s.max_queued = max_queued;
	s.signal     = [](void* ctx, int index) { SetEvent(static_cast<objects*>(ctx)->events[index]); };
	s.signal_ctx = _o.get();
	_ring        = datapath::detail::make_unique<ring_pair>(s);
}

datapath::windows::shm_channel::~shm_channel() = default;

HANDLE datapath::windows::shm_channel::data_packet() const noexcept
{
	return _o->packets[0];
}
HANDLE datapath::windows::shm_channel::space_packet() const noexcept
{
	return _o->packets[1];
}
HANDLE datapath::windows::shm_channel::rx_data_event() const noexcept
{
	return _o->events[_ring->rx() * 2 + sig_data];
}
HANDLE datapath::windows::shm_channel::tx_space_event() const noexcept
{
	return _o->events[_ring->tx() * 2 + sig_space];
}

void datapath::windows::shm_channel::wake_self() noexcept
{
	SetEvent(rx_data_event());
	SetEvent(tx_space_event());
}

// ---------------------------------------------------------------- setup

datapath::error datapath::windows::shm_channel::create(std::unique_ptr<shm_channel>& out, HANDLE pipe,
													   uint64_t ring_size, uint64_t limit, uint64_t max_queued,
													   bool zero_copy, uint8_t (&offer)[shm_offer_size])
{
	if (!supported())
		return datapath::error::NotSupported;
	std::wstring own  = current_user_sid();
	std::wstring peer = pipe_client_sid(pipe);
	if (own.empty() || peer.empty())
		return datapath::error::AccessDenied;

	uint64_t cap = ring_capacity(ring_size);

	auto o  = datapath::detail::make_unique<objects>();
	o->cap  = cap;
	o->other_user = peer != own;
	o->zero_copy  = zero_copy;

	// Owner: everything. Peer: map read/write + query + read control (0x20007)
	// on the section, modify state + synchronize (0x100002) on the events.
	PSECURITY_DESCRIPTOR sd_sec = nullptr, sd_ev = nullptr;
	auto                 make_sd = [&](const wchar_t* mask, PSECURITY_DESCRIPTOR& sd) {
        std::wstring sddl = L"D:P(A;;GA;;;" + own + L")(A;;" + mask + L";;;" + peer + L")";
        return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)
               != FALSE;
	};
	bool ok = make_sd(L"0x20007", sd_sec) && make_sd(L"0x100002", sd_ev);
	SECURITY_ATTRIBUTES sa_sec{sizeof(sa_sec), sd_sec, FALSE}, sa_ev{sizeof(sa_ev), sd_ev, FALSE};

	uint8_t rnd[16] = {};
	ok              = ok && api().gen_random(rnd, sizeof(rnd));
	char hex[33]    = {};
	for (int i = 0; i < 16; i++) {
		static const char digits[] = "0123456789abcdef";
		hex[2 * i]                 = digits[rnd[i] >> 4];
		hex[2 * i + 1]             = digits[rnd[i] & 15];
	}

	// Global objects reach clients in other sessions (a service and a desktop
	// app); creating them needs SeCreateGlobalPrivilege, so fall back to Local.
	uint64_t total  = data_offset + 2 * cap;
	char     scope  = 'G';
	std::wstring base;
	if (ok) {
		for (char s : {'G', 'L'}) {
			base       = std::wstring(s == 'G' ? L"Global\\dpshm-" : L"Local\\dpshm-") + std::wstring(hex, hex + 32);
			o->section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa_sec, PAGE_READWRITE | SEC_COMMIT,
											DWORD(total >> 32), DWORD(total), (base + suffixes[0]).c_str());
			if (o->section && GetLastError() == ERROR_ALREADY_EXISTS) {
				CloseHandle(o->section);
				o->section = nullptr;
				ok         = false; // 128 random bits collided: someone is squatting
				break;
			}
			if (o->section) {
				scope = s;
				break;
			}
		}
		ok = ok && o->section;
	}
	for (int i = 0; ok && i < 4; i++) {
		o->events[i] = CreateEventW(&sa_ev, FALSE, FALSE, (base + suffixes[i + 1]).c_str());
		ok           = o->events[i] && GetLastError() != ERROR_ALREADY_EXISTS;
	}
	if (sd_sec)
		LocalFree(sd_sec);
	if (sd_ev)
		LocalFree(sd_ev);
	ok = ok && (o->view = MapViewOfFile(o->section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0)) != nullptr;
	ok = ok && o->make_packets();
	if (!ok)
		return datapath::error::Failure;

	uint32_t flags = 0;
	if (o->map_mirrors())
		flags = flag_mirror;
	else
		for (auto& m : o->mirror)
			m.release(cap);
	init_header(o->view, cap, flags);

	std::memset(offer, 0, sizeof(offer));
	std::memcpy(offer, "DPSM", 4);
	datapath::detail::store_le32(offer + 4, version);
	datapath::detail::store_le64(offer + 8, cap);
	datapath::detail::store_le32(offer + 16, flags);
	offer[24] = uint8_t(scope);
	std::memcpy(offer + 25, hex, 32);

	out.reset(new shm_channel(std::move(o), true, limit, max_queued));
	return datapath::error::Success;
}

datapath::error datapath::windows::shm_channel::open(std::unique_ptr<shm_channel>& out,
													 HANDLE pipe, const uint8_t (&offer)[shm_offer_size],
													 uint64_t limit, uint64_t max_queued, bool zero_copy)
{
	if (!supported())
		return datapath::error::NotSupported;
	uint64_t cap   = datapath::detail::load_le64(offer + 8);
	uint32_t flags = datapath::detail::load_le32(offer + 16);
	if (std::memcmp(offer, "DPSM", 4) != 0 || datapath::detail::load_le32(offer + 4) != version
		|| cap < min_ring || cap > max_ring || (cap & (cap - 1)) != 0 || (flags & ~flag_mirror) != 0
		|| (offer[24] != 'G' && offer[24] != 'L') || !is_hex_name(offer + 25, 32))
		return datapath::error::VersionMismatch;

	auto o = datapath::detail::make_unique<objects>();
	o->cap = cap;
	std::wstring base =
		std::wstring(offer[24] == 'G' ? L"Global\\dpshm-" : L"Local\\dpshm-") + std::wstring(offer + 25, offer + 57);
	o->section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (base + suffixes[0]).c_str());
	if (!o->section)
		return datapath::error::AccessDenied;
	for (int i = 0; i < 4; i++)
		if (!(o->events[i] = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (base + suffixes[i + 1]).c_str())))
			return datapath::error::AccessDenied;

	// Copy messages out before delivery unless zero-copy was asked for and the
	// server process runs as our user. (Not the section's owner: for an
	// elevated server that is the Administrators group.) If the server's user
	// can't be read, it counts as another user.
	o->zero_copy      = zero_copy;
	std::wstring peer = pipe_server_sid(pipe);
	o->other_user     = peer.empty() || peer != current_user_sid();
	if (!(o->view = MapViewOfFile(o->section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0)))
		return datapath::error::Failure;
	// The server sized the section: make sure it holds both rings before using it.
	MEMORY_BASIC_INFORMATION mbi{};
	if (!VirtualQuery(o->view, &mbi, sizeof(mbi)) || mbi.RegionSize < data_offset + 2 * cap
		|| !header_matches(o->view, cap, flags))
		return datapath::error::VersionMismatch;
	if ((flags & flag_mirror) && !o->map_mirrors())
		return datapath::error::NotSupported;
	if (!o->make_packets())
		return datapath::error::Failure;
	out.reset(new shm_channel(std::move(o), false, limit, max_queued));
	return datapath::error::Success;
}

