/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER lttng_ust_libc

#if !defined(_TRACEPOINT_UST_LIBC_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _TRACEPOINT_UST_LIBC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_libc, malloc,
	LTTNG_UST_TP_ARGS(size_t, size, void *, ptr, void *, ip),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(size_t, size, size)
		lttng_ust_field_integer_hex(void *, ptr, ptr)
		lttng_ust_field_unused(ip)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_libc, free,
	LTTNG_UST_TP_ARGS(void *, ptr, void *, ip),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(void *, ptr, ptr)
		lttng_ust_field_unused(ip)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_libc, calloc,
	LTTNG_UST_TP_ARGS(size_t, nmemb, size_t, size, void *, ptr, void *, ip),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(size_t, nmemb, nmemb)
		lttng_ust_field_integer(size_t, size, size)
		lttng_ust_field_integer_hex(void *, ptr, ptr)
		lttng_ust_field_unused(ip)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_libc, realloc,
	LTTNG_UST_TP_ARGS(void *, in_ptr, size_t, size, void *, ptr, void *, ip),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(void *, in_ptr, in_ptr)
		lttng_ust_field_integer(size_t, size, size)
		lttng_ust_field_integer_hex(void *, ptr, ptr)
		lttng_ust_field_unused(ip)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_libc, memalign,
	LTTNG_UST_TP_ARGS(size_t, alignment, size_t, size, void *, ptr, void *, ip),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(size_t, alignment, alignment)
		lttng_ust_field_integer(size_t, size, size)
		lttng_ust_field_integer_hex(void *, ptr, ptr)
		lttng_ust_field_unused(ip)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(lttng_ust_libc, posix_memalign,
	LTTNG_UST_TP_ARGS(void *, out_ptr, size_t, alignment, size_t, size, int, result, void *, ip),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(void *, out_ptr, out_ptr)
		lttng_ust_field_integer(size_t, alignment, alignment)
		lttng_ust_field_integer(size_t, size, size)
		lttng_ust_field_integer(int, result, result)
		lttng_ust_field_unused(ip)
	)
)

#endif /* _TRACEPOINT_UST_LIBC_H */

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./ust_libc.h"

/* This part must be outside ifdef protection */
#include <lttng/tracepoint-event.h>

#ifdef __cplusplus
}
#endif
