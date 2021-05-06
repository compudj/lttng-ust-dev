/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2013 Jérémie Galarneau
 */

#define TRACEPOINT_DEFINE
#define TRACEPOINT_CREATE_PROBES
#include "ust_tests_sameline.h"

int main(void)
{
	tracepoint(ust_tests_sameline, event1);	tracepoint(ust_tests_sameline, event2);
	return 0;
}
