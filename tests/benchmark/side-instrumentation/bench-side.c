/*
 * SPDX-FileCopyrightText: 2026 EfficiOS, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Emit one libside event carrying a single 32-bit integer, in a loop.
 *
 * This program depends on libside only: it does not link against
 * LTTng-UST. run-benchmark preloads the tracer, which subscribes to the
 * event and records it, so what is timed is the whole path from the
 * instrumentation site to the committed record.
 *
 * The loop runs twice: once to prime the ring buffer pages and settle
 * the caches and the branch predictors, then once which is timed.
 *
 * bench-tp.c is the same program written against an LTTng-UST
 * tracepoint, which is what these numbers are compared to.
 */

#define _LGPL_SOURCE

#include <side/trace.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

side_static_event(bench_event, "side_benchmark", "u32", SIDE_LOGLEVEL_INFO,
	side_field_list(
		side_field_u32("v"),
	)
);

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/*
 * noinline so that the loop is not folded into the timing code, and so
 * that it shows up as a symbol of its own in a profile.
 */
__attribute__((noinline))
static void emit(uint64_t nr)
{
	uint64_t i;

	for (i = 0; i < nr; i++) {
		side_event(bench_event, side_arg_list(side_arg_u32((uint32_t) i)));
	}
}

int main(int argc, char **argv)
{
	uint64_t warmup = 1000000, iters = 5000000, reps = 7, r;

	if (argc > 1)
		iters = strtoull(argv[1], NULL, 10);
	if (argc > 2)
		warmup = strtoull(argv[2], NULL, 10);
	if (argc > 3)
		reps = strtoull(argv[3], NULL, 10);

	emit(warmup);

	for (r = 0; r < reps; r++) {
		uint64_t begin, end;

		begin = now_ns();
		emit(iters);
		end = now_ns();
		/* Nanoseconds per event, one line per repetition. */
		printf("%.2f\n", (double) (end - begin) / (double) iters);
		fflush(stdout);
	}
	return 0;
}
