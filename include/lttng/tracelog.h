/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2013-2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_UST_TRACELOG_H
#define _LTTNG_UST_TRACELOG_H

#include <lttng/tp/lttng-ust-tracelog.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TP_TRACELOG_CB_TEMPLATE(level)					\
	__attribute__ ((format(printf, 4, 5)))				\
	extern void _lttng_ust_tracelog_##level(const char *file,	\
		int line, const char *func, const char *fmt, ...);	\
	__attribute__ ((format(printf, 4, 0)))				\
	extern void _lttng_ust_vtracelog_##level(const char *file,	\
		int line, const char *func, const char *fmt, va_list ap);

TP_TRACELOG_CB_TEMPLATE(TRACE_EMERG);
TP_TRACELOG_CB_TEMPLATE(TRACE_ALERT);
TP_TRACELOG_CB_TEMPLATE(TRACE_CRIT);
TP_TRACELOG_CB_TEMPLATE(TRACE_ERR);
TP_TRACELOG_CB_TEMPLATE(TRACE_WARNING);
TP_TRACELOG_CB_TEMPLATE(TRACE_NOTICE);
TP_TRACELOG_CB_TEMPLATE(TRACE_INFO);
TP_TRACELOG_CB_TEMPLATE(TRACE_DEBUG_SYSTEM);
TP_TRACELOG_CB_TEMPLATE(TRACE_DEBUG_PROGRAM);
TP_TRACELOG_CB_TEMPLATE(TRACE_DEBUG_PROCESS);
TP_TRACELOG_CB_TEMPLATE(TRACE_DEBUG_MODULE);
TP_TRACELOG_CB_TEMPLATE(TRACE_DEBUG_UNIT);
TP_TRACELOG_CB_TEMPLATE(TRACE_DEBUG_FUNCTION);
TP_TRACELOG_CB_TEMPLATE(TRACE_DEBUG_LINE);
TP_TRACELOG_CB_TEMPLATE(TRACE_DEBUG);

#undef TP_TRACELOG_CB_TEMPLATE

#define tracelog(level, fmt, ...)					\
	do {								\
		LTTNG_STAP_PROBEV(tracepoint_lttng_ust_tracelog, level, ## __VA_ARGS__); \
		if (caa_unlikely(__tracepoint_lttng_ust_tracelog___##level.state)) \
			_lttng_ust_tracelog_##level(__FILE__, __LINE__, __func__, \
				fmt, ## __VA_ARGS__);			\
	} while (0)

#define vtracelog(level, fmt, ap)					\
	do {								\
		if (caa_unlikely(__tracepoint_lttng_ust_tracelog___##level.state)) \
			_lttng_ust_vtracelog_##level(__FILE__, __LINE__, __func__, \
				fmt, ap);				\
	} while (0)

#ifdef __cplusplus
}
#endif

#endif /* _LTTNG_UST_TRACELOG_H */
