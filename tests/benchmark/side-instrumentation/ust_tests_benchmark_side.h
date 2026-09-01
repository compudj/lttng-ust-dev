/*
 * SPDX-FileCopyrightText: 2026 EfficiOS, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * The tracepoint this benchmark compares libside against: one event
 * carrying a single 32-bit integer, which is what bench-side.c emits
 * through libside.
 */

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER tp_benchmark

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./ust_tests_benchmark_side.h"

#if !defined(_TRACEPOINT_UST_TESTS_BENCHMARK_SIDE_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _TRACEPOINT_UST_TESTS_BENCHMARK_SIDE_H

#include <lttng/tracepoint.h>
#include <stdint.h>

LTTNG_UST_TRACEPOINT_EVENT(
	tp_benchmark,
	u32,
	LTTNG_UST_TP_ARGS(uint32_t, v),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(uint32_t, v, v)
	)
)

LTTNG_UST_TRACEPOINT_LOGLEVEL(tp_benchmark, u32, LTTNG_UST_TRACEPOINT_LOGLEVEL_INFO)

#endif /* _TRACEPOINT_UST_TESTS_BENCHMARK_SIDE_H */

#include <lttng/tracepoint-event.h>
