/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER lttng_ust_libc

#if !defined(_TRACEPOINT_UST_LIBC_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _TRACEPOINT_UST_LIBC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(lttng_ust_libc, malloc,
	TP_ARGS(size_t, size, void *, ptr, void *, ip),
	TP_FIELDS(
		ctf_integer(size_t, size, size)
		ctf_integer_hex(void *, ptr, ptr)
	)
)

TRACEPOINT_EVENT(lttng_ust_libc, free,
	TP_ARGS(void *, ptr, void *, ip),
	TP_FIELDS(
		ctf_integer_hex(void *, ptr, ptr)
	)
)

TRACEPOINT_EVENT(lttng_ust_libc, calloc,
	TP_ARGS(size_t, nmemb, size_t, size, void *, ptr, void *, ip),
	TP_FIELDS(
		ctf_integer(size_t, nmemb, nmemb)
		ctf_integer(size_t, size, size)
		ctf_integer_hex(void *, ptr, ptr)
	)
)

TRACEPOINT_EVENT(lttng_ust_libc, realloc,
	TP_ARGS(void *, in_ptr, size_t, size, void *, ptr, void *, ip),
	TP_FIELDS(
		ctf_integer_hex(void *, in_ptr, in_ptr)
		ctf_integer(size_t, size, size)
		ctf_integer_hex(void *, ptr, ptr)
	)
)

TRACEPOINT_EVENT(lttng_ust_libc, memalign,
	TP_ARGS(size_t, alignment, size_t, size, void *, ptr, void *, ip),
	TP_FIELDS(
		ctf_integer(size_t, alignment, alignment)
		ctf_integer(size_t, size, size)
		ctf_integer_hex(void *, ptr, ptr)
	)
)

TRACEPOINT_EVENT(lttng_ust_libc, posix_memalign,
	TP_ARGS(void *, out_ptr, size_t, alignment, size_t, size, int, result, void *, ip),
	TP_FIELDS(
		ctf_integer_hex(void *, out_ptr, out_ptr)
		ctf_integer(size_t, alignment, alignment)
		ctf_integer(size_t, size, size)
		ctf_integer(int, result, result)
	)
)

#endif /* _TRACEPOINT_UST_LIBC_H */

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "./ust_libc.h"

/* This part must be outside ifdef protection */
#include <lttng/tracepoint-event.h>

#ifdef __cplusplus
}
#endif
