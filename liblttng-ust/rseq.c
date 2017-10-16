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

#define _GNU_SOURCE
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

#include <rseq.h>

#define ARRAY_SIZE(arr)	(sizeof(arr) / sizeof((arr)[0]))

__attribute__((weak)) __thread volatile struct rseq __rseq_abi = {
	.u.e.cpu_id = -1,
};

/* Own state, not shared with other libs. */
static __thread int rseq_registered;

static pthread_key_t rseq_key;

#ifdef __NR_rseq
static int sys_rseq(volatile struct rseq *rseq_abi, int flags, uint32_t sig)
{
	return syscall(__NR_rseq, rseq_abi, flags, sig);
}
#else
static int sys_rseq(volatile struct rseq *rseq_abi, int flags, uint32_t sig)
{
	errno = ENOSYS;
	return -1;
}
#endif

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

int rseq_unregister_current_thread(void)
{
	sigset_t oldset;
	int rc, ret = 0;

	signal_off_save(&oldset);
	if (rseq_registered) {
		rc = sys_rseq(&__rseq_abi, RSEQ_FLAG_UNREGISTER, RSEQ_SIG);
		if (rc) {
			fprintf(stderr, "Error: sys_rseq(...) failed(%d): %s\n",
				errno, strerror(errno));
			ret = -1;
			goto end;
		}
		rseq_registered = 0;
	}
end:
	signal_restore(oldset);
	return ret;
}

static void destroy_rseq_key(void *key)
{
	if (rseq_unregister_current_thread())
		abort();
}

int rseq_register_current_thread(void)
{
	sigset_t oldset;
	int rc, ret = 0;

	signal_off_save(&oldset);
	if (caa_likely(!rseq_registered)) {
		rc = sys_rseq(&__rseq_abi, 0, RSEQ_SIG);
		if (rc) {
			fprintf(stderr, "Error: sys_rseq(...) failed(%d): %s\n",
				errno, strerror(errno));
			__rseq_abi.u.e.cpu_id = -2;
			ret = -1;
			goto end;
		}
		rseq_registered = 1;
		assert(rseq_current_cpu_raw() >= 0);
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

int rseq_fallback_current_cpu(void)
{
	int cpu;

	cpu = sched_getcpu();
	if (cpu < 0) {
		perror("sched_getcpu()");
		abort();
	}
	return cpu;
}

void rseq_init(void)
{
	int ret;

	ret = pthread_key_create(&rseq_key, destroy_rseq_key);
	if (ret) {
		errno = -ret;
		perror("pthread_key_create");
		abort();
	}
}

void rseq_destroy(void)
{
	int ret;

	ret = pthread_key_delete(rseq_key);
	if (ret) {
		errno = -ret;
		perror("pthread_key_delete");
		abort();
	}
}
