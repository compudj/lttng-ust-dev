/*
 * bench.c
 *
 * LTTng Userspace Tracer (UST) - benchmark tool
 *
 * Copyright 2010 - Douglas Santos <douglas.santos@polymtl.ca>
 * Copyright 2021 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <urcu/compiler.h>

#ifdef TRACING
#define TRACEPOINT_DEFINE
#include "ust_tests_benchmark.h"
#endif

#define printf_verbose(fmt, args...)		\
	do {					\
		if (verbose_mode)		\
			printf(fmt, ## args);	\
	} while (0)

static int verbose_mode;

struct thread_counter {
	unsigned long long nr_loops;
};

static int nr_threads;
static unsigned long duration;

static volatile int test_go, test_stop;

void do_stuff(void)
{
	int i;
#ifdef TRACING
	int v = 50;
#endif

	for (i = 0; i < 100; i++)
		cmm_barrier();
#ifdef TRACING
	tracepoint(ust_tests_benchmark, tpbench, v);
#endif
}

void *function(void *arg)
{
	unsigned long long nr_loops = 0;
	struct thread_counter *thread_counter = arg;

	while (!test_go)
		cmm_barrier();

	for (;;) {
		do_stuff();
		nr_loops++;
		if (test_stop)
			break;
	}
	thread_counter->nr_loops = nr_loops;
	return NULL;
}

void usage(char **argv) {
	printf("Usage: %s nr_threads duration(s) <OPTIONS>\n", argv[0]);
	printf("OPTIONS:\n");
	printf("        [-v] (verbose output)\n");
	printf("\n");
}

int main(int argc, char **argv)
{
	unsigned long long total_loops = 0;
	unsigned long i_thr;
	void *retval;
	int i;

	if (argc < 3) {
		usage(argv);
		exit(1);
	}

	nr_threads = atoi(argv[1]);
	duration = atol(argv[2]);

	for (i = 3; i < argc; i++) {
		if (argv[i][0] != '-')
			continue;
		switch (argv[i][1]) {
		case 'v':
			verbose_mode = 1;
			break;
		}
	}

	printf_verbose("using %d thread(s)\n", nr_threads);
	printf_verbose("for a duration of %lds\n", duration);

	pthread_t thread[nr_threads];
	struct thread_counter thread_counter[nr_threads];

	for (i = 0; i < nr_threads; i++) {
		thread_counter[i].nr_loops = 0;
		if (pthread_create(&thread[i], NULL, function, &thread_counter[i])) {
			fprintf(stderr, "thread create %d failed\n", i);
			exit(1);
		}
	}

	test_go = 1;

	for (i_thr = 0; i_thr < duration; i_thr++) {
		sleep(1);
		if (verbose_mode) {
			fwrite(".", sizeof(char), 1, stdout);
			fflush(stdout);
		}
	}
	printf_verbose("\n");

	test_stop = 1;

	for (i = 0; i < nr_threads; i++) {
		if (pthread_join(thread[i], &retval)) {
			fprintf(stderr, "thread join %d failed\n", i);
			exit(1);
		}
		total_loops += thread_counter[i].nr_loops;
	}
	printf("Number of loops: %llu\n", total_loops);
	return 0;
}
