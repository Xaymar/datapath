/*
Low Latency IPC Library for high-speed traffic
Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>

SPDX-License-Identifier: BSD-3-Clause
*/

// Minimal self-registering test harness. No third-party dependencies.
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); }
// The executable runs every test, or only the one named by argv[1].

#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace check {
	struct failure {
		std::string message;
	};

	struct entry {
		const char*           name;
		std::function<void()> fn;
	};

	inline std::vector<entry>& registry()
	{
		static std::vector<entry> r;
		return r;
	}

	struct registrar {
		registrar(const char* name, std::function<void()> fn)
		{
			registry().push_back({name, std::move(fn)});
		}
	};

	template<typename T>
	std::string show(const T& v)
	{
		std::ostringstream ss;
		if constexpr (std::is_enum_v<T>) {
			ss << static_cast<long long>(v);
		} else {
			ss << v;
		}
		return ss.str();
	}

	[[noreturn]] inline void fail(const char* file, int line, const std::string& what)
	{
		std::ostringstream ss;
		ss << file << ":" << line << ": " << what;
		throw failure{ss.str()};
	}

	inline int run(int argc, char** argv)
	{
		int ran = 0, failed = 0;
		for (auto& e : registry()) {
			if (argc > 1 && std::strcmp(argv[1], e.name) != 0)
				continue;
			ran++;
			try {
				e.fn();
				std::printf("[ PASS ] %s\n", e.name);
			} catch (const failure& f) {
				failed++;
				std::printf("[ FAIL ] %s\n         %s\n", e.name, f.message.c_str());
			} catch (const std::exception& ex) {
				failed++;
				std::printf("[ FAIL ] %s\n         exception: %s\n", e.name, ex.what());
			}
			std::fflush(stdout);
		}
		if (ran == 0) {
			std::printf("no test matched\n");
			return 2;
		}
		return failed ? 1 : 0;
	}
} // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                         \
	static void           test_##name();                                   \
	static check::registrar CHECK_CAT(reg_, name)(#name, &test_##name);    \
	static void           test_##name()
#define CHECK(expr)                                                        \
	do {                                                                   \
		if (!(expr))                                                       \
			check::fail(__FILE__, __LINE__, "CHECK(" #expr ") failed");    \
	} while (0)
#define CHECK_EQ(a, b)                                                                                     \
	do {                                                                                                   \
		auto&& check_a_ = (a);                                                                             \
		auto&& check_b_ = (b);                                                                             \
		if (!(check_a_ == check_b_))                                                                       \
			check::fail(__FILE__, __LINE__,                                                                \
						"CHECK_EQ(" #a ", " #b ") failed: " + check::show(check_a_) + " != " + check::show(check_b_)); \
	} while (0)
#define TEST_MAIN()                      \
	int main(int argc, char** argv)      \
	{                                    \
		return check::run(argc, argv);   \
	}
