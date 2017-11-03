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

/* Own state, not shared with other libs. */
//TODO: TLS IE
static __thread int rseq_registered;

static pthread_key_t rseq_key;

static void signal_off_save(sigset_t *oldset)
{
	sigset_t set;
	int ret;

	sigfillset(&set);
	ret = pthread_sigmask(SIG_BLOCK, &set, oldset);
	if (ret)
		abort();
}

static void signal_restore(sigset_t oldset)
{
	int ret;

	ret = pthread_sigmask(SIG_SETMASK, &oldset, NULL);
	if (ret)
		abort();
}

static int lttng_rseq_unregister_current_thread(void)
{
	sigset_t oldset;
	int rc, ret = 0;

	signal_off_save(&oldset);
	if (rseq_registered) {
		rc = rseq_unregister_current_thread();
		if (rc) {
			ret = -1;
			goto end;
		}
		rseq_registered = 0;
	}
end:
	signal_restore(oldset);
	return ret;
}

static void lttng_destroy_rseq_key(void *key)
{
	if (lttng_rseq_unregister_current_thread())
		abort();
}

int lttng_rseq_register_current_thread(void)
{
	sigset_t oldset;
	int rc, ret = 0;

	signal_off_save(&oldset);
	if (caa_likely(!rseq_registered)) {
		rc = rseq_register_current_thread();
		if (rc) {
			ret = -1;
			goto end;
		}
		rseq_registered = 1;
		/*
		 * Register destroy notifier. Pointer needs to
		 * be non-NULL.
		 */
		if (pthread_setspecific(rseq_key, (void *)0x1))
			abort();
	}
end:
	signal_restore(oldset);
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
