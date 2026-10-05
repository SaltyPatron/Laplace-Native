/* A minimal test harness: CHECK counts failures, and the test exits nonzero if any failed. */
#ifndef LP_CHECK_H
#define LP_CHECK_H
#include <stdio.h>
#include <stdlib.h>

static int lp_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { lp_failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                                                fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define DONE(name) do { printf("%s: %s\n", name, lp_failures ? "FAILED" : "ok"); return lp_failures ? 1 : 0; } while (0)

#endif
