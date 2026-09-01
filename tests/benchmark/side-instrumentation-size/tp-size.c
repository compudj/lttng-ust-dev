/*
 * SPDX-FileCopyrightText: 2026 EfficiOS, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * The same application written against LTTng-UST tracepoints: the
 * generated event definitions, and with CALLSITES defined, one call
 * site per event. tp-probes.c compiles the probe providers into the
 * program, which is what a tracepoint needs and what libside has no
 * equivalent of.
 */

#define _LGPL_SOURCE
#define LTTNG_UST_TRACEPOINT_DEFINE

#include "tp-events.h"

int main(void)
{
#ifdef CALLSITES
#include "tp-callsites.h"
#endif
	return 0;
}
