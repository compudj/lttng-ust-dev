/*
 * rseq.c
 *
 * Copyright (C) 2016 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <assert.h>
#include <signal.h>
#include <urcu/compiler.h>

#include <urcu/rseq.h>

static pthread_key_t rseq_key;

static void lttng_destroy_rseq_key(void *key)
{
	if (rseq_unregister_current_thread())
		abort();
}

int lttng_rseq_register_current_thread(void)
{
	int rc, ret = 0;

	rc = rseq_register_current_thread();
	if (rc) {
		ret = -1;
		goto end;
	}
	/*
	 * Register destroy notifier. Pointer needs to
	 * be non-NULL.
	 */
	if (pthread_setspecific(rseq_key, (void *)0x1))
		abort();
end:
	return ret;
}

void lttng_ust_rseq_init(void)
{
	int ret;

	ret = pthread_key_create(&rseq_key, lttng_destroy_rseq_key);
	if (ret) {
		errno = -ret;
		perror("pthread_key_create");
		abort();
	}
}

void lttng_ust_rseq_destroy(void)
{
	int ret;

	ret = pthread_key_delete(rseq_key);
	if (ret) {
		errno = -ret;
		perror("pthread_key_delete");
		abort();
	}
}
