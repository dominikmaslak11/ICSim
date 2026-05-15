/* Minimal C test runner — no external dependencies. */
#ifndef ICSIM_TESTS_RUNNER_H
#define ICSIM_TESTS_RUNNER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _tests_run = 0;
static int _tests_fail = 0;

#define TEST(name) \
	do { \
		int _assert_fail = 0; \
		_tests_run++; \
		printf("  %-50s ", name); \
		fflush(stdout); \
		{

#define END_TEST \
		} \
		if (!_assert_fail) \
			printf("\033[32mPASS\033[0m\n"); \
		else \
			printf("\033[31mFAIL\033[0m\n"); \
	} while(0)

#define ASSERT(cond) \
	do { \
		if (!(cond)) { \
			printf("\n    ASSERT failed at %s:%d: %s\n", \
			       __FILE__, __LINE__, #cond); \
			_assert_fail = 1; \
			_tests_fail++; \
		} \
	} while(0)

#define ASSERT_INT_EQ(a, b) \
	do { \
		int _va = (a), _vb = (b); \
		if (_va != _vb) { \
			printf("\n    ASSERT_INT_EQ failed at %s:%d: " \
			       "%s(%d) != %s(%d)\n", \
			       __FILE__, __LINE__, #a, _va, #b, _vb); \
			_assert_fail = 1; \
			_tests_fail++; \
		} \
	} while(0)

#define ASSERT_STREQ(a, b) \
	do { \
		const char *_va = (a), *_vb = (b); \
		if (strcmp(_va, _vb) != 0) { \
			printf("\n    ASSERT_STREQ failed at %s:%d: " \
			       "\"%s\" != \"%s\"\n", \
			       __FILE__, __LINE__, _va, _vb); \
			_assert_fail = 1; \
			_tests_fail++; \
		} \
	} while(0)

static int test_summary(void)
{
	if (_tests_fail == 0) {
		printf("\n\033[32mAll %d tests passed\033[0m\n", _tests_run);
		return 0;
	}
	printf("\n\033[31mFAIL: %d/%d tests failed\033[0m\n",
	       _tests_fail, _tests_run);
	return 1;
}

#endif /* ICSIM_TESTS_RUNNER_H */
