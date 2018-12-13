#ifndef _LTTNG_UST_RCU_H
#define _LTTNG_UST_RCU_H

/*
 * lttng/ust-rcu.h
 *
 * LTTng-UST RCU header
 *
 * RCU read-side adapted for tracing library. Does not require thread
 * registration nor unregistration. Also signal-safe.
 *
 * Copyright (c) 2009-2018 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
 *
 * LGPL-compatible code should include this header with :
 *
 * #define _LGPL_SOURCE
 * #include <lttng/lttng-ust-rcu.h>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * IBM's contributions to this file may be relicensed under LGPLv2 or later.
 */

#ifndef _LGPL_SOURCE
#error "lttng-ust-rcu.h is only meant to be included from LGPL code."
#endif

#include <stdlib.h>
#include <pthread.h>
#include <urcu/list.h>
#include <urcu/tls-compat.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * See urcu/pointer.h and urcu/static/pointer.h for pointer
 * publication headers.
 */
#include <urcu/pointer.h>

enum lttng_ust_rcu_state {
	LTTNG_UST_RCU_READER_ACTIVE_CURRENT,
	LTTNG_UST_RCU_READER_ACTIVE_OLD,
	LTTNG_UST_RCU_READER_INACTIVE,
};

/*
 * The trick here is that LTTNG_UST_RCU_GP_CTR_PHASE must be a multiple
 * of 8 so we can use a full 8-bits, 16-bits or 32-bits bitmask for the
 * lower order bits.
 */
#define LTTNG_UST_RCU_GP_COUNT		(1UL << 0)
/* Use the amount of bits equal to half of the architecture long size */
#define LTTNG_UST_RCU_GP_CTR_PHASE	(1UL << (sizeof(long) << 2))
#define LTTNG_UST_RCU_GP_CTR_NEST_MASK	(LTTNG_UST_RCU_GP_CTR_PHASE - 1)

//TODO: add CONFIG_LTTNG_UST_RCU_DEBUG to configure.ac and config.h.in
#ifdef CONFIG_LTTNG_UST_RCU_DEBUG
#define lttng_ust_rcu_assert(...)	assert(__VA_ARGS__)
#else
#define lttng_ust_rcu_assert(...)
#endif

extern void lttng_ust_synchronize_rcu(void);

/*
 * lttng_ust_rcu_before_fork, lttng_ust_rcu_after_fork_parent and
 * lttng_ust_rcu_after_fork_child should be called around fork() system
 * calls when the child process is not expected to immediately perform
 * an exec(). For pthread users, see pthread_atfork(3).
 */
extern void lttng_ust_rcu_before_fork(void);
extern void lttng_ust_rcu_after_fork_parent(void);
extern void lttng_ust_rcu_after_fork_child(void);

/*
 * Used internally by lttng_ust_rcu_read_lock.
 */
extern void lttng_ust_rcu_register(void);

struct lttng_ust_rcu_gp {
	/*
	 * Global grace period counter.
	 * Contains the current LTTNG_UST_RCU_GP_CTR_PHASE.
	 * Also has a LTTNG_UST_RCU_GP_COUNT of 1, to accelerate the
	 * reader fast path.  Written to only by writer with mutex
	 * taken. Read by both writer and readers.
	 */
	unsigned long ctr;
} __attribute__((aligned(CAA_CACHE_LINE_SIZE)));

extern struct lttng_ust_rcu_gp lttng_ust_rcu_gp;

struct lttng_ust_rcu_reader {
	/* Data used by both reader and lttng_ust_synchronize_rcu() */
	unsigned long ctr;
	/* Data used for registry */
	struct cds_list_head node __attribute__((aligned(CAA_CACHE_LINE_SIZE)));
	pthread_t tid;
	int alloc;	/* registry entry allocated */
};

/*
 * LTTng UST RCU keeps a pointer to a registry not part of the TLS.
 * Adds a pointer dereference on the read-side, but won't require to unregister
 * the reader thread.
 */
extern DECLARE_URCU_TLS(struct lttng_ust_rcu_reader *, lttng_ust_rcu_reader);

#ifdef CONFIG_LTTNG_UST_FORCE_SYS_MEMBARRIER
#define lttng_ust_rcu_has_sys_membarrier	1
#else
extern int lttng_ust_rcu_has_sys_membarrier;
#endif

extern const struct rcu_flavor_struct lttng_ust_rcu_flavor;

static inline void lttng_ust_rcu_smp_mb_slave(void)
{
	if (caa_likely(lttng_ust_rcu_has_sys_membarrier))
		cmm_barrier();
	else
		cmm_smp_mb();
}

static inline
enum lttng_ust_rcu_state lttng_ust_rcu_reader_state(unsigned long *ctr)
{
	unsigned long v;

	if (ctr == NULL)
		return LTTNG_UST_RCU_READER_INACTIVE;
	/*
	 * Make sure both tests below are done on the same version of *value
	 * to insure consistency.
	 */
	v = CMM_LOAD_SHARED(*ctr);
	if (!(v & LTTNG_UST_RCU_GP_CTR_NEST_MASK))
		return LTTNG_UST_RCU_READER_INACTIVE;
	if (!((v ^ lttng_ust_rcu_gp.ctr) & LTTNG_UST_RCU_GP_CTR_PHASE))
		return LTTNG_UST_RCU_READER_ACTIVE_CURRENT;
	return LTTNG_UST_RCU_READER_ACTIVE_OLD;
}

/*
 * Helper for lttng_ust_rcu_read_lock().  The format of
 * lttng_ust_rcu_gp.ctr (as well as the per-thread rcu_reader.ctr) has
 * the upper bits containing a count of lttng_ust_rcu_read_lock()
 * nesting, and a lower-order bit that contains either zero or
 * LTTNG_UST_RCU_GP_CTR_PHASE.  The smp_mb_slave() ensures that the
 * accesses in lttng_ust_rcu_read_lock() happen before the subsequent
 * read-side critical section.
 */
static inline void lttng_ust_rcu_read_lock_update(unsigned long tmp)
{
	if (caa_likely(!(tmp & LTTNG_UST_RCU_GP_CTR_NEST_MASK))) {
		_CMM_STORE_SHARED(URCU_TLS(lttng_ust_rcu_reader)->ctr,
			_CMM_LOAD_SHARED(lttng_ust_rcu_gp.ctr));
		lttng_ust_rcu_smp_mb_slave();
	} else
		_CMM_STORE_SHARED(URCU_TLS(lttng_ust_rcu_reader)->ctr,
			tmp + LTTNG_UST_RCU_GP_COUNT);
}

/*
 * Enter an RCU read-side critical section.
 *
 * The first cmm_barrier() call ensures that the compiler does not reorder
 * the body of lttng_ust_rcu_read_lock() with a mutex.
 *
 * This function and its helper are both less than 10 lines long.  The
 * intent is that this function meets the 10-line criterion in LGPL,
 * allowing this function to be invoked directly from non-LGPL code.
 */
static inline void lttng_ust_rcu_read_lock(void)
{
	unsigned long tmp;

	if (caa_unlikely(!URCU_TLS(lttng_ust_rcu_reader)))
		lttng_ust_rcu_register(); /* If not yet registered. */
	cmm_barrier();	/* Ensure the compiler does not reorder us with mutex */
	tmp = URCU_TLS(lttng_ust_rcu_reader)->ctr;
	lttng_ust_rcu_assert((tmp & LTTNG_UST_RCU_GP_CTR_NEST_MASK) != LTTNG_UST_RCU_GP_CTR_NEST_MASK);
	lttng_ust_rcu_read_lock_update(tmp);
}

/*
 * Exit an RCU read-side critical section.  This function is less than
 * 10 lines of code, and is intended to be usable by non-LGPL code, as
 * called out in LGPL.
 */
static inline void lttng_ust_rcu_read_unlock(void)
{
	unsigned long tmp;

	tmp = URCU_TLS(lttng_ust_rcu_reader)->ctr;
	lttng_ust_rcu_assert(tmp & LTTNG_UST_RCU_GP_CTR_NEST_MASK);
	/* Finish using rcu before decrementing the pointer. */
	lttng_ust_rcu_smp_mb_slave();
	_CMM_STORE_SHARED(URCU_TLS(lttng_ust_rcu_reader)->ctr, tmp - LTTNG_UST_RCU_GP_COUNT);
	cmm_barrier();	/* Ensure the compiler does not reorder us with mutex */
}

/*
 * Returns whether within a RCU read-side critical section.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline int lttng_ust_rcu_read_ongoing(void)
{
	if (caa_unlikely(!URCU_TLS(lttng_ust_rcu_reader)))
		lttng_ust_rcu_register(); /* If not yet registered. */
	return URCU_TLS(lttng_ust_rcu_reader)->ctr & LTTNG_UST_RCU_GP_CTR_NEST_MASK;
}

#ifdef __cplusplus
}
#endif

#include <urcu/flavor.h>

#endif /* _LTTNG_UST_RCU_H */
