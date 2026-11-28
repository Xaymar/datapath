/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

#include "shm.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include "common/endian.hpp"
#include "platform.hpp"

#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/syscall.h>
#endif

#if defined(__linux__) && defined(SYS_memfd_create)
#define DATAPATH_SHM_POSIX 1
// Older C library headers may lack these (values from linux/memfd.h, fcntl.h).
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#endif
#else
#define DATAPATH_SHM_POSIX 0
#endif

namespace {
	using namespace datapath::detail::shm;

	void close_all(int (&fds)[datapath::posix::shm_fd_count]) noexcept
	{
		for (int& fd : fds) {
			if (fd >= 0)
				::close(fd);
			fd = -1;
		}
	}

#if DATAPATH_SHM_POSIX
	// One ring mapped twice, back to back, so that a record running past the end
	// lands at its start.
	uint8_t* map_mirror(int fd, uint64_t offset, uint64_t cap)
	{
		void* base = ::mmap(nullptr, size_t(2 * cap), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (base == MAP_FAILED)
			return nullptr;
		for (int i = 0; i < 2; i++) {
			void* want = static_cast<uint8_t*>(base) + i * cap;
			void* got  = ::mmap(want, size_t(cap), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(offset));
			if (got != want) {
				::munmap(base, size_t(2 * cap));
				return nullptr;
			}
		}
		return static_cast<uint8_t*>(base);
	}

	// What a descriptor the server sent really is: an eventfd shows up as
	// "anon_inode:[eventfd]". True when that can't be checked (no /proc).
	bool looks_like_eventfd(int fd)
	{
		char        link[64] = {};
		std::string path     = "/proc/self/fd/" + std::to_string(fd);
		ssize_t     n        = ::readlink(path.c_str(), link, sizeof(link) - 1);
		if (n < 0)
			return true;
		return std::strcmp(link, "anon_inode:[eventfd]") == 0;
	}
#endif
} // namespace

struct datapath::posix::shm_link::objects {
	int      fds[shm_fd_count] = {-1, -1, -1, -1, -1};
	void*    header            = nullptr;
	uint8_t* mirror[2]         = {};
	uint64_t cap               = 0;

	~objects()
	{
		for (auto* m : mirror)
			if (m)
				::munmap(m, size_t(2 * cap));
		if (header)
			::munmap(header, size_t(data_offset));
		close_all(fds);
	}

#if DATAPATH_SHM_POSIX
	bool map()
	{
		void* h = ::mmap(nullptr, size_t(data_offset), PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
		if (h == MAP_FAILED)
			return false;
		header = h;
		for (int r = 0; r < 2; r++)
			if (!(mirror[r] = map_mirror(fds[0], data_offset + r * cap, cap)))
				return false;
		return true;
	}
#endif
};

bool datapath::posix::shm_link::supported() noexcept
{
	return DATAPATH_SHM_POSIX != 0;
}

bool datapath::posix::shm_link::wanted(const datapath::options& opts) noexcept
{
	static const int forced = [] {
		const char* v = std::getenv("DATAPATH_SHARED_MEMORY");
		return v && *v ? (v[0] == '0' ? 0 : 1) : -1;
	}();
	bool want = forced < 0 ? opts.shared_memory : forced == 1;
	return want && supported();
}

datapath::posix::shm_link::shm_link(std::unique_ptr<objects> o, bool server, bool other_user, bool zero_copy,
									 uint64_t limit, uint64_t max_queued)
	: _o(std::move(o))
{
	ring_pair::setup s{};
	s.header     = static_cast<section_header*>(_o->header);
	s.data[0]    = _o->mirror[0];
	s.data[1]    = _o->mirror[1];
	s.cap        = _o->cap;
	s.mirrored   = true;
	s.other_user = other_user;
	s.zero_copy  = zero_copy;
	s.server     = server;
	s.limit      = limit;
	s.max_queued = max_queued;
	s.signal     = [](void* ctx, int index) {
        uint64_t one = 1;
        ssize_t  r   = ::write(static_cast<objects*>(ctx)->fds[1 + index], &one, sizeof(one));
        (void)r; // EAGAIN: the counter is already non-zero, the reader wakes anyway
	};
	s.signal_ctx = _o.get();
	// Same section, same inode: the ThreadSanitizer key both ends agree on.
	static char tsan_keys[256][2];
	struct stat st {};
	if (::fstat(_o->fds[0], &st) == 0) {
		s.sync_key[0] = &tsan_keys[st.st_ino % 256][0];
		s.sync_key[1] = &tsan_keys[st.st_ino % 256][1];
	}
	_ring = datapath::detail::make_unique<ring_pair>(s);
}

datapath::posix::shm_link::~shm_link() = default;

int datapath::posix::shm_link::rx_data_fd() const noexcept
{
	return _o->fds[1 + _ring->rx() * 2 + sig_data];
}

int datapath::posix::shm_link::tx_space_fd() const noexcept
{
	return _o->fds[1 + _ring->tx() * 2 + sig_space];
}

void datapath::posix::shm_link::clear(int efd) noexcept
{
	uint64_t v;
	ssize_t  r = ::read(efd, &v, sizeof(v));
	(void)r;
}

datapath::error datapath::posix::shm_link::create(std::unique_ptr<shm_link>& out, int sock, uint64_t ring_size,
												  uint64_t limit, uint64_t max_queued, bool zero_copy,
												  uint8_t (&offer)[shm_offer_size], int (&fds)[shm_fd_count])
{
#if DATAPATH_SHM_POSIX
	uid_t uid = 0;
	gid_t gid = 0;
	if (!peer_credentials(sock, uid, gid))
		return datapath::error::AccessDenied;
	auto o   = datapath::detail::make_unique<objects>();
	o->cap   = ring_capacity(ring_size);
	int  mfd = int(::syscall(SYS_memfd_create, "datapath-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING));
	if (mfd < 0)
		return datapath::error::NotSupported;
	o->fds[0] = mfd;
	if (::ftruncate(mfd, off_t(data_offset + 2 * o->cap)) != 0
		|| ::fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
		return datapath::error::Failure;
	for (int i = 1; i < shm_fd_count; i++)
		if ((o->fds[i] = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0)
			return datapath::error::Failure;
	if (!o->map())
		return datapath::error::Failure;
	init_header(o->header, o->cap, flag_mirror);

	std::memset(offer, 0, sizeof(offer));
	std::memcpy(offer, "DPSM", 4);
	datapath::detail::store_le32(offer + 4, version);
	datapath::detail::store_le64(offer + 8, o->cap);
	datapath::detail::store_le32(offer + 16, flag_mirror);
	for (int i = 0; i < shm_fd_count; i++)
		fds[i] = o->fds[i];
	out.reset(new shm_link(std::move(o), true, uid != ::geteuid(), zero_copy, limit, max_queued));
	return datapath::error::Success;
#else
	(void)out, (void)sock, (void)ring_size, (void)limit, (void)max_queued, (void)zero_copy, (void)offer, (void)fds;
	return datapath::error::NotSupported;
#endif
}

datapath::error datapath::posix::shm_link::open(std::unique_ptr<shm_link>& out, int sock,
												const uint8_t (&offer)[shm_offer_size], int (&fds)[shm_fd_count],
												uint64_t limit, uint64_t max_queued, bool zero_copy)
{
	auto o = datapath::detail::make_unique<objects>();
	for (int i = 0; i < shm_fd_count; i++) {
		o->fds[i] = fds[i];
		fds[i]    = -1;
	}
#if DATAPATH_SHM_POSIX
	uint64_t cap   = datapath::detail::load_le64(offer + 8);
	uint32_t flags = datapath::detail::load_le32(offer + 16);
	if (std::memcmp(offer, "DPSM", 4) != 0 || datapath::detail::load_le32(offer + 4) != version || cap < min_ring
		|| cap > max_ring || (cap & (cap - 1)) != 0 || flags != flag_mirror)
		return datapath::error::VersionMismatch;
	for (int fd : o->fds)
		if (fd < 0)
			return datapath::error::VersionMismatch;
	o->cap = cap;

	// The section: a memfd of the right size that can no longer shrink.
	struct stat st {};
	int         seals = ::fcntl(o->fds[0], F_GET_SEALS);
	if (::fstat(o->fds[0], &st) != 0 || !S_ISREG(st.st_mode) || uint64_t(st.st_size) < data_offset + 2 * cap
		|| seals < 0 || !(seals & F_SEAL_SHRINK))
		return datapath::error::VersionMismatch;
	// The wake-ups: eventfds, never blocking this side.
	for (int i = 1; i < shm_fd_count; i++) {
		int fl = ::fcntl(o->fds[i], F_GETFL);
		if (fl < 0 || !looks_like_eventfd(o->fds[i]) || ::fcntl(o->fds[i], F_SETFL, fl | O_NONBLOCK) != 0)
			return datapath::error::VersionMismatch;
	}
	if (!o->map() || !header_matches(o->header, cap, flags))
		return datapath::error::VersionMismatch;

	uid_t uid = 0;
	gid_t gid = 0;
	if (!peer_credentials(sock, uid, gid))
		return datapath::error::AccessDenied;
	out.reset(new shm_link(std::move(o), false, uid != ::geteuid(), zero_copy, limit, max_queued));
	return datapath::error::Success;
#else
	(void)out, (void)sock, (void)offer, (void)limit, (void)max_queued, (void)zero_copy;
	return datapath::error::NotSupported;
#endif
}
