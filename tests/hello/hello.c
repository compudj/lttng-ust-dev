/*
 * Copyright (C) 2009  Pierre-Marc Fournier
 * Copyright (C) 2011  Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1 of
 * the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
/*
 * Work-around inet.h missing struct mmsghdr forward declaration, with
 * triggers a warning when system files warnings are enabled.
 */
struct mmsghdr;
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdbool.h>

#define TRACEPOINT_DEFINE
#include "ust_tests_hello.h"

void inthandler(int sig)
{
	printf("in SIGUSR1 handler\n");
	tracepoint(ust_tests_hello, tptest_sighandler);
}

int init_int_handler(void)
{
	int result;
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	result = sigemptyset(&act.sa_mask);
	if (result == -1) {
		perror("sigemptyset");
		return -1;
	}

	act.sa_handler = inthandler;
	act.sa_flags = SA_RESTART;

	/* Only defer ourselves. Also, try to restart interrupted
	 * syscalls to disturb the traced program as little as possible.
	 */
	result = sigaction(SIGUSR1, &act, NULL);
	if (result == -1) {
		perror("sigaction");
		return -1;
	}

	return 0;
}

void test_inc_count(void);

int main(int argc, char **argv)
{
	int i, netint;
	long values[] = { 1, 2, 3 };
	char text[10] = "test";
	double dbl = 2.0;
	float flt = 2222.0;
	int delay = 0;
	bool mybool = 123;	/* should print "1" */

	init_int_handler();

	if (argc == 2)
		delay = atoi(argv[1]);

	fprintf(stderr, "Hello, World!\n");

	sleep(delay);

	fprintf(stderr, "Tracing... ");
	for (i = 0; i < 1000000; i++) {
		netint = htonl(i);
		tracepoint(ust_tests_hello, tptest, i, netint, values,
			   text, strlen(text), dbl, flt, mybool);
		//usleep(100000);
	}
	fprintf(stderr, " done.\n");
	return 0;
}
