/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2011-2014 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <lttng/tracepoint.h>
#include <stdarg.h>

TRACEPOINT_EVENT(lttng_ust_tracef, event,
	TP_ARGS(const char *, msg, unsigned int, len, void *, ip),
	TP_FIELDS(
		ctf_sequence_text(char, msg, msg, unsigned int, len)
		ctf_unused(ip)
	)
)
TRACEPOINT_LOGLEVEL(lttng_ust_tracef, event, TRACE_DEBUG)
