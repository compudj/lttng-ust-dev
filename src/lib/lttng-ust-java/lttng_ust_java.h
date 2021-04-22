/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2011  Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER lttng_ust_java

#if !defined(_TRACEPOINT_LTTNG_UST_JAVA_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _TRACEPOINT_LTTNG_UST_JAVA_H

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_java, int_event,
	LTTNG_UST_TP_ARGS(const char *, name, int, payload),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(name, name)
		lttng_ust_field_integer(int, int_payload, payload)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_java, int_int_event,
	LTTNG_UST_TP_ARGS(const char *, name, int, payload1, int, payload2),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(name, name)
		lttng_ust_field_integer(int, int_payload1, payload1)
		lttng_ust_field_integer(int, int_payload2, payload2)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_java, long_event,
	LTTNG_UST_TP_ARGS(const char *, name, long, payload),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(name, name)
		lttng_ust_field_integer(long, long_payload, payload)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_java, long_long_event,
	LTTNG_UST_TP_ARGS(const char *, name, long, payload1, long, payload2),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(name, name)
		lttng_ust_field_integer(long, long_payload1, payload1)
		lttng_ust_field_integer(long, long_payload2, payload2)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_java, string_event,
	LTTNG_UST_TP_ARGS(const char *, name, const char *, payload),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(name, name)
		lttng_ust_field_string(string_payload, payload)
	)
)

#endif /* _TRACEPOINT_LTTNG_UST_JAVA_H */

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./lttng_ust_java.h"

/* This part must be outside protection */
#include <lttng/tracepoint-event.h>
