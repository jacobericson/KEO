#ifndef KEO_TOOLS_TESTS_CHECK_H
#define KEO_TOOLS_TESTS_CHECK_H

// Host-suite assertions. The failure count is a function-local static of an inline
// function, so every translation unit of one suite shares it.

#include <cstdio>

inline int& CheckFailureCounter()
{
	static int failures = 0;
	return failures;
}

inline int CheckFailureCount()
{
	return CheckFailureCounter();
}

inline void Check(bool ok, const char* what)
{
	if (!ok)
	{
		++CheckFailureCounter();
		std::printf("FAIL %s\n", what);
	}
}

#define CHECK(cond, what) Check((cond) ? true : false, (what))

// The suite's exit code: 0 when no check failed.
inline int CheckExit(const char* suite)
{
	int n = CheckFailureCount();
	if (n == 0)
		std::printf("%s: all checks passed\n", suite);
	else
		std::printf("%s: %d failure(s)\n", suite, n);
	return n == 0 ? 0 : 1;
}

#endif
