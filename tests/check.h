/* Tiny assertion helper shared by the tests. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <errno.h>

static int failures;

#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #c, errno); \
	failures++; } } while (0)

#define TEST_DONE(name) do { \
	puts(failures ? name ": FAILED" : name ": PASS"); \
	return failures != 0; } while (0)

#endif
