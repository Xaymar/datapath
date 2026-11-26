/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Language and library compatibility. The library builds with C++11 or newer:
// where a newer standard library has a facility it is used, otherwise a small
// local fallback takes its place. Detection uses feature-test macros rather
// than __cplusplus, because toolchains with partial C++17 support report the
// language level while still missing library parts.

#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#if __has_include(<shared_mutex>)
#include <shared_mutex>
#endif
#endif

#if defined(__cpp_lib_shared_mutex) && __cpp_lib_shared_mutex >= 201505L
#define DATAPATH_HAS_STD_SHARED_MUTEX 1
#else
#define DATAPATH_HAS_STD_SHARED_MUTEX 0
#if defined(_WIN32)
#include "windows/win.hpp"
#else
#include <pthread.h>
#include <sched.h>
#endif
#endif

#if defined(__cpp_lib_enable_shared_from_this) && __cpp_lib_enable_shared_from_this >= 201603L
#define DATAPATH_HAS_WEAK_FROM_THIS 1
#else
#define DATAPATH_HAS_WEAK_FROM_THIS 0
#endif

#if defined(__cpp_lib_make_unique) && __cpp_lib_make_unique >= 201304L
#define DATAPATH_HAS_MAKE_UNIQUE 1
#else
#define DATAPATH_HAS_MAKE_UNIQUE 0
#endif

#if defined(__cpp_lib_atomic_is_always_lock_free) && __cpp_lib_atomic_is_always_lock_free >= 201603L
#define DATAPATH_ATOMIC_ALWAYS_LOCK_FREE(T) (std::atomic<T>::is_always_lock_free)
#else
// Pre-C++17 equivalent: 2 means "always lock-free" for every object of that type.
#define DATAPATH_ATOMIC_ALWAYS_LOCK_FREE(T)                       \
	((sizeof(T) == sizeof(int) && ATOMIC_INT_LOCK_FREE == 2)      \
	 || (sizeof(T) == sizeof(long) && ATOMIC_LONG_LOCK_FREE == 2) \
	 || (sizeof(T) == sizeof(long long) && ATOMIC_LLONG_LOCK_FREE == 2))
#endif

namespace datapath { namespace detail {
#if DATAPATH_HAS_MAKE_UNIQUE
	using std::make_unique;
#else
	template<typename T, typename... Args>
	std::unique_ptr<T> make_unique(Args&&... args)
	{
		return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
	}
#endif

	// Reader/writer lock. std::shared_mutex where the library has it, otherwise
	// the platform primitive std::shared_mutex is built on (SRWLOCK on MSVC,
	// pthread_rwlock with libstdc++). Neither throws nor aborts.
#if DATAPATH_HAS_STD_SHARED_MUTEX
	using shared_mutex = std::shared_mutex;
#elif defined(_WIN32)
	class shared_mutex {
		SRWLOCK _lock = SRWLOCK_INIT;

		public:
		shared_mutex()                               = default;
		shared_mutex(const shared_mutex&)            = delete;
		shared_mutex& operator=(const shared_mutex&) = delete;

		void lock() noexcept
		{
			AcquireSRWLockExclusive(&_lock);
		}
		void unlock() noexcept
		{
			ReleaseSRWLockExclusive(&_lock);
		}
		void lock_shared() noexcept
		{
			AcquireSRWLockShared(&_lock);
		}
		void unlock_shared() noexcept
		{
			ReleaseSRWLockShared(&_lock);
		}
	};
#else
	class shared_mutex {
		pthread_rwlock_t _lock = PTHREAD_RWLOCK_INITIALIZER;

		public:
		shared_mutex() = default;
		~shared_mutex()
		{
			pthread_rwlock_destroy(&_lock);
		}
		shared_mutex(const shared_mutex&)            = delete;
		shared_mutex& operator=(const shared_mutex&) = delete;

		// EDEADLK cannot happen (no recursive locking); EAGAIN (too many
		// readers) is retried, like libstdc++ does.
		void lock() noexcept
		{
			while (pthread_rwlock_wrlock(&_lock) != 0)
				sched_yield();
		}
		void unlock() noexcept
		{
			pthread_rwlock_unlock(&_lock);
		}
		void lock_shared() noexcept
		{
			while (pthread_rwlock_rdlock(&_lock) != 0)
				sched_yield();
		}
		void unlock_shared() noexcept
		{
			pthread_rwlock_unlock(&_lock);
		}
	};
#endif

	// Scoped shared ownership of a shared_mutex (std::shared_lock is C++14).
	template<typename M>
	class shared_lock {
		M*   _m;
		bool _owns;

		public:
		explicit shared_lock(M& m) : _m(&m), _owns(true)
		{
			m.lock_shared();
		}
		~shared_lock()
		{
			if (_owns)
				_m->unlock_shared();
		}
		shared_lock(const shared_lock&)            = delete;
		shared_lock& operator=(const shared_lock&) = delete;

		void unlock()
		{
			_m->unlock_shared();
			_owns = false;
		}
	};

	// enable_shared_from_this plus weak_from_this() on every standard. Before
	// C++17 the weak reference is stored when the object is created through
	// make_shared_self(), which every user of weak_from_this() must use.
	template<typename T>
	class enable_weak_from_this : public std::enable_shared_from_this<T> {
#if !DATAPATH_HAS_WEAK_FROM_THIS
		std::weak_ptr<T> _weak_self;

		template<typename U, typename... Args>
		friend std::shared_ptr<U> make_shared_self(Args&&... args);

		public:
		std::weak_ptr<T> weak_from_this() noexcept
		{
			return _weak_self;
		}
		std::weak_ptr<const T> weak_from_this() const noexcept
		{
			return _weak_self;
		}
#endif
	};

	template<typename T, typename... Args>
	std::shared_ptr<T> make_shared_self(Args&&... args)
	{
		std::shared_ptr<T> p = std::make_shared<T>(std::forward<Args>(args)...);
#if !DATAPATH_HAS_WEAK_FROM_THIS
		p->_weak_self = p;
#endif
		return p;
	}
}} // namespace datapath::detail
