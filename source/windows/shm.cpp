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
	constexpr uint32_t shm_magic   = 0x4d535044; // "DPSM"
	constexpr uint32_t shm_version = 1;
	constexpr uint64_t rec_more    = uint64_t(1) << 32;
	constexpr uint64_t rec_pad     = uint64_t(1) << 33; // low 32 bits: bytes to skip (0 = to the ring start)
	constexpr uint64_t min_ring    = uint64_t(64) << 10;
	constexpr uint64_t max_ring    = uint64_t(1) << 30;
	constexpr uint64_t page        = 4096;
	constexpr uint32_t hdr_mirror  = 1;

	struct alignas(64) ring_ctl {
		alignas(64) std::atomic<uint64_t> head;           // bytes published by the writer
		alignas(64) std::atomic<uint64_t> tail;           // bytes released by the reader
		alignas(64) std::atomic<uint32_t> reader_waiting; // reader is (about to be) blocked on its event
		alignas(64) std::atomic<uint32_t> writer_waiting; // writer is (about to be) blocked for space
	};

	struct section_header {
		uint32_t magic;
		uint32_t version;
		uint64_t capacity;
		uint32_t flags;
		ring_ctl ring[2]; // [0] server -> client, [1] client -> server
	};
	static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared atomics must be lock-free");

	// Rings start on the 64 KiB allocation granularity so each can be mapped on
	// its own (twice, mirrored).
	constexpr uint64_t data_offset = 64 * 1024;
	static_assert(sizeof(section_header) <= data_offset, "header must fit before the rings");

	const wchar_t* const suffixes[5] = {L"-m", L"-d0", L"-s0", L"-d1", L"-s1"};

	uint64_t align8(uint64_t v) noexcept
	{
		return (v + 7) & ~uint64_t(7);
	}

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
	bool        copy       = true;
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

// Walks the parts of one message.
struct datapath::windows::shm_channel::cursor {
	const buffer_view* parts;
	size_t             count;
	size_t             i   = 0;
	size_t             off = 0;

	const uint8_t* ptr()
	{
		while (i < count && off == parts[i].size)
			i++, off = 0;
		return i < count ? static_cast<const uint8_t*>(parts[i].data) + off : nullptr;
	}
	void copy(uint8_t* dst, uint64_t n)
	{
		while (n) {
			const uint8_t* p = ptr();
			size_t         k = size_t(std::min<uint64_t>(n, parts[i].size - off));
			std::memcpy(dst, p, k);
			dst += k;
			off += k;
			n -= k;
		}
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
	: _o(std::move(o)), _limit(limit), _max_queued(max_queued)
{
	_cap    = _o->cap;
	_mirror = _o->mirrored;
	_copy   = _o->copy;
	_tx     = server ? 0 : 1;
	_rx     = server ? 1 : 0;
	if (_mirror) {
		_data[0] = _o->mirror[0].base;
		_data[1] = _o->mirror[1].base;
	} else {
		_data[0] = static_cast<uint8_t*>(_o->view) + data_offset;
		_data[1] = _data[0] + _cap;
	}
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
	return _o->events[_rx * 2];
}
HANDLE datapath::windows::shm_channel::tx_space_event() const noexcept
{
	return _o->events[_tx * 2 + 1];
}

void datapath::windows::shm_channel::wake_self() noexcept
{
	SetEvent(rx_data_event());
	SetEvent(tx_space_event());
}

// ---------------------------------------------------------------- setup

datapath::error datapath::windows::shm_channel::create(std::unique_ptr<shm_channel>& out, HANDLE pipe,
													   uint64_t ring_size, uint64_t limit, uint64_t max_queued,
													   uint8_t (&offer)[shm_offer_size])
{
	if (!supported())
		return datapath::error::NotSupported;
	std::wstring own  = current_user_sid();
	std::wstring peer = pipe_client_sid(pipe);
	if (own.empty() || peer.empty())
		return datapath::error::AccessDenied;

	uint64_t cap = min_ring;
	while (cap < ring_size && cap < max_ring)
		cap <<= 1;

	auto o  = std::make_unique<objects>();
	o->cap  = cap;
	o->copy = peer != own;

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

	auto* hdr     = new (o->view) section_header{};
	hdr->magic    = shm_magic;
	hdr->version  = shm_version;
	hdr->capacity = cap;
	if (o->map_mirrors())
		hdr->flags = hdr_mirror;
	else
		for (auto& m : o->mirror)
			m.release(cap);

	std::memset(offer, 0, sizeof(offer));
	std::memcpy(offer, "DPSM", 4);
	datapath::detail::store_le32(offer + 4, shm_version);
	datapath::detail::store_le64(offer + 8, cap);
	datapath::detail::store_le32(offer + 16, hdr->flags);
	offer[24] = uint8_t(scope);
	std::memcpy(offer + 25, hex, 32);

	out.reset(new shm_channel(std::move(o), true, limit, max_queued));
	return datapath::error::Success;
}

datapath::error datapath::windows::shm_channel::open(std::unique_ptr<shm_channel>& out,
													 const uint8_t (&offer)[shm_offer_size], uint64_t limit,
													 uint64_t max_queued)
{
	if (!supported())
		return datapath::error::NotSupported;
	uint64_t cap   = datapath::detail::load_le64(offer + 8);
	uint32_t flags = datapath::detail::load_le32(offer + 16);
	if (std::memcmp(offer, "DPSM", 4) != 0 || datapath::detail::load_le32(offer + 4) != shm_version
		|| cap < min_ring || cap > max_ring || (cap & (cap - 1)) != 0 || (flags & ~hdr_mirror) != 0
		|| (offer[24] != 'G' && offer[24] != 'L') || !is_hex_name(offer + 25, 32))
		return datapath::error::VersionMismatch;

	auto o = std::make_unique<objects>();
	o->cap = cap;
	std::wstring base =
		std::wstring(offer[24] == 'G' ? L"Global\\dpshm-" : L"Local\\dpshm-") + std::wstring(offer + 25, offer + 57);
	o->section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (base + suffixes[0]).c_str());
	if (!o->section)
		return datapath::error::AccessDenied;
	for (int i = 0; i < 4; i++)
		if (!(o->events[i] = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (base + suffixes[i + 1]).c_str())))
			return datapath::error::AccessDenied;

	// Copy messages out before delivery unless the section's owner is our user.
	PSID                 owner = nullptr;
	PSECURITY_DESCRIPTOR sd    = nullptr;
	if (GetSecurityInfo(o->section, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr,
						&sd)
		== ERROR_SUCCESS) {
		o->copy = sid_string(owner) != current_user_sid();
		LocalFree(sd);
	}
	if (!(o->view = MapViewOfFile(o->section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0)))
		return datapath::error::Failure;
	// The server sized the section: make sure it holds both rings before using it.
	MEMORY_BASIC_INFORMATION mbi{};
	auto*                    hdr = o->header();
	if (!VirtualQuery(o->view, &mbi, sizeof(mbi)) || mbi.RegionSize < data_offset + 2 * cap || hdr->magic != shm_magic
		|| hdr->version != shm_version || hdr->capacity != cap || hdr->flags != flags)
		return datapath::error::VersionMismatch;
	if ((flags & hdr_mirror) && !o->map_mirrors())
		return datapath::error::NotSupported;
	if (!o->make_packets())
		return datapath::error::Failure;
	out.reset(new shm_channel(std::move(o), false, limit, max_queued));
	return datapath::error::Success;
}

// ---------------------------------------------------------------- writer

namespace {
	// Bytes of padding in front of a record whose header would go at `pos`, so
	// that a payload of 4 KiB or more lands at the source's offset in its page.
	uint64_t gap_for(uint64_t pos, const uint8_t* src, uint64_t len) noexcept
	{
		if (len < page || !src)
			return 0;
		uint64_t want = (reinterpret_cast<uintptr_t>(src) & (page - 1)) & ~uint64_t(7);
		uint64_t cur  = (pos + 8) & (page - 1);
		return (want - cur) & (page - 1);
	}
} // namespace

uint64_t datapath::windows::shm_channel::free_space()
{
	auto&    tx   = _o->header()->ring[_tx];
	uint64_t used = _head - tx.tail.load(std::memory_order_acquire);
	if (used > _cap) {
		_broken = true; // the peer wrote an impossible position
		return 0;
	}
	return _cap - used;
}

void datapath::windows::shm_channel::wake_reader()
{
	auto& tx = _o->header()->ring[_tx];
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (tx.reader_waiting.load(std::memory_order_relaxed) && tx.reader_waiting.exchange(0))
		SetEvent(_o->events[_tx * 2]);
}

bool datapath::windows::shm_channel::put(cursor& c, uint64_t len, bool more)
{
	const uint8_t* src  = c.ptr();
	uint64_t       need = 8 + align8(len);
	uint64_t       pos  = _head & (_cap - 1);
	uint64_t       gap  = gap_for(pos, src, len);
	uint64_t       f    = free_space();
	uint64_t       total;
	if (!_mirror && _cap - pos < gap + need)
		total = (_cap - pos) + gap_for(0, src, len) + need;
	else
		total = gap + need;
	if (_broken || f < total)
		return false;
	uint8_t* d = _data[_tx];
	if (!_mirror && _cap - pos < gap + need) {
		uint64_t pad = rec_pad; // to the ring start
		std::memcpy(d + pos, &pad, 8);
		_head += _cap - pos;
		pos = 0;
		gap = gap_for(0, src, len);
	}
	if (gap) {
		uint64_t pad = rec_pad | gap;
		std::memcpy(d + pos, &pad, 8);
		_head += gap;
		pos = _head & (_cap - 1);
	}
	uint64_t h = len | (more ? rec_more : 0);
	std::memcpy(d + pos, &h, 8);
	c.copy(d + pos + 8, len);
	_head += need;
	_o->header()->ring[_tx].head.store(_head, std::memory_order_release);
	wake_reader();
	return true;
}

// Write fragments of the message at `c` (`total` bytes left) while they fit.
// `complete` is set once its last fragment is in the ring. An empty message is
// one empty record, so it is delivered like any other.
bool datapath::windows::shm_channel::write_message(cursor& c, uint64_t total, bool& complete, size_t& written)
{
	uint64_t frag_max = (_mirror ? _cap - 8 : _cap / 2 - 8) - page;
	written           = 0;
	complete          = false;
	do {
		uint64_t n = std::min<uint64_t>(total - written, frag_max);
		if (!put(c, n, written + n < total))
			break;
		written += size_t(n);
		complete = written == total;
	} while (!complete);
	return !_broken;
}

datapath::error datapath::windows::shm_channel::send(const buffer_view* parts, size_t count, bool& need_space)
{
	uint64_t total = 0;
	for (size_t i = 0; i < count; i++)
		total += parts[i].size;
	if (total > _limit)
		return datapath::error::TooLarge;
	std::lock_guard<std::mutex> l(_send_lock);
	need_space = false;
	if (_broken)
		return datapath::error::Closed;
	cursor c{parts, count};
	size_t written  = 0;
	bool   complete = false;
	if (_pending.empty()) {
		if (!write_message(c, total, complete, written))
			return datapath::error::Closed;
		if (complete)
			return datapath::error::Success;
	} else if (_pending_bytes + total > _max_queued) {
		return datapath::error::WouldBlock;
	}
	if (written == 0 && _pending_bytes + total > _max_queued)
		return datapath::error::WouldBlock;
	// Keep the rest (a started message must be finished whatever the limit).
	pending p;
	p.bytes.resize(size_t(total - written));
	c.copy(p.bytes.data(), total - written);
	_pending_bytes += p.bytes.size();
	_pending.push_back(std::move(p));
	need_space = true;
	return datapath::error::Success;
}

bool datapath::windows::shm_channel::drain(bool& need_space)
{
	std::lock_guard<std::mutex> l(_send_lock);
	while (!_pending.empty() && !_broken) {
		auto&       p = _pending.front();
		buffer_view v{p.bytes.data() + p.off, p.bytes.size() - p.off};
		cursor      c{&v, 1};
		size_t      written  = 0;
		bool        complete = false;
		write_message(c, v.size, complete, written);
		p.off += written;
		_pending_bytes -= written;
		if (!complete)
			break;
		_pending.pop_front();
	}
	need_space = !_pending.empty() && !_broken;
	return !_broken;
}

bool datapath::windows::shm_channel::prepare_space_wait()
{
	auto& tx = _o->header()->ring[_tx];
	tx.writer_waiting.store(1, std::memory_order_seq_cst);
	bool need = false;
	drain(need); // the reader may have made room before it saw the flag
	if (!need)
		tx.writer_waiting.store(0, std::memory_order_relaxed);
	return need;
}

// ---------------------------------------------------------------- reader

bool datapath::windows::shm_channel::prepare_block()
{
	auto& rx = _o->header()->ring[_rx];
	rx.reader_waiting.store(1, std::memory_order_seq_cst);
	if (rx.head.load(std::memory_order_seq_cst) != _tail) {
		rx.reader_waiting.store(0, std::memory_order_relaxed);
		return false;
	}
	return true;
}

int datapath::windows::shm_channel::poll_impl(bool (*deliver)(void*, const uint8_t*, size_t), void* ctx)
{
	auto& rx = _o->header()->ring[_rx];
	if (rx.reader_waiting.load(std::memory_order_relaxed))
		rx.reader_waiting.store(0, std::memory_order_relaxed);
	uint64_t h = rx.head.load(std::memory_order_acquire);
	if (h == _tail)
		return 0;
	if (h - _tail > _cap)
		return -1;
	uint64_t       frag_max = (_mirror ? _cap - 8 : _cap / 2 - 8) - page;
	const uint8_t* d        = _data[_rx];
	while (_tail != h) {
		uint64_t pos = _tail & (_cap - 1);
		uint64_t rec;
		std::memcpy(&rec, d + pos, 8); // read once: the peer may change it
		uint64_t step;
		bool     stop = false;
		if (rec & rec_pad) {
			uint64_t gap = rec & 0xffffffffu;
			bool     ok;
			if (gap) {
				step = gap;
				ok   = gap % 8 == 0 && gap < page && (_mirror || step <= _cap - pos);
			} else {
				step = _cap - pos;
				ok   = !_mirror && pos != 0;
			}
			if (!ok || step > h - _tail)
				return -1;
		} else {
			uint64_t len  = rec & 0xffffffffu;
			bool     more = (rec & rec_more) != 0;
			step          = 8 + align8(len);
			if (step > h - _tail || len > frag_max || (!_mirror && step > _cap - pos))
				return -1;
			const uint8_t* p = d + pos + 8;
			if (more || !_assembly.empty()) {
				if (_assembly.size() + len > _limit)
					return -1;
				_assembly.insert(_assembly.end(), p, p + len);
				if (!more) {
					stop = !deliver(ctx, _assembly.data(), _assembly.size());
					_assembly.clear();
					if (_assembly.capacity() > (size_t(4) << 20))
						_assembly.shrink_to_fit();
				}
			} else {
				if (len > _limit)
					return -1;
				if (_copy) {
					_scratch.assign(p, p + len);
					p = _scratch.data();
				}
				stop = !deliver(ctx, p, size_t(len));
			}
		}
		_tail += step;
		rx.tail.store(_tail, std::memory_order_release);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (rx.writer_waiting.load(std::memory_order_relaxed) && rx.writer_waiting.exchange(0))
			SetEvent(_o->events[_rx * 2 + 1]);
		if (stop)
			break;
	}
	return 1;
}
