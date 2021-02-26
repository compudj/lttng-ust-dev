/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2011-2012 Matthew Khouzam <matthew.khouzam@ericsson.com>
 * Copyright (C) 2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <unistd.h>

/*
 * We need to define TRACEPOINT_DEFINE in one C file in the program
 * before including provider headers.
 */
#define TRACEPOINT_DEFINE
#include "sample_component_provider.h"

int main(int argc, char **argv)
{
	int i = 0;

	for (i = 0; i < 100000; i++) {
		tracepoint(sample_component, message, "Hello World");
		usleep(1);
	}
	return 0;
}
