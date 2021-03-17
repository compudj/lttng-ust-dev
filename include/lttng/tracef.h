/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2013-2014 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_UST_TRACEF_H
#define _LTTNG_UST_TRACEF_H

#include <lttng/tp/lttng-ust-tracef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern
void _lttng_ust_tracef(const char *fmt, ...);

extern
void _lttng_ust_vtracef(const char *fmt, va_list ap);

#define tracef(fmt, ...)						\
	do {								\
		LTTNG_STAP_PROBEV(tracepoint_lttng_ust_tracef, event, ## __VA_ARGS__); \
		if (caa_unlikely(__tracepoint_lttng_ust_tracef___event.state)) \
			_lttng_ust_tracef(fmt, ## __VA_ARGS__);		\
	} while (0)

#define vtracef(fmt, ap)						\
	do {								\
		if (caa_unlikely(__tracepoint_lttng_ust_tracef___event.state)) \
			_lttng_ust_vtracef(fmt, ap);		\
	} while (0)
#ifdef __cplusplus
}
#endif

#endif /* _LTTNG_UST_TRACEF_H */
