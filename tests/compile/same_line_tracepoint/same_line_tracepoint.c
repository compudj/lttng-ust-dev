/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2013 Jérémie Galarneau
 */

#define LTTNG_UST_TRACEPOINT_DEFINE
#define LTTNG_UST_TRACEPOINT_CREATE_PROBES
#include "ust_tests_sameline.h"

int main(void)
{
	lttng_ust_tracepoint(ust_tests_sameline, event1);	lttng_ust_tracepoint(ust_tests_sameline, event2);
	return 0;
}
