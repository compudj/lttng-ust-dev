#ifndef _LTTNG_RING_BUFFER_VATOMIC_H
#define _LTTNG_RING_BUFFER_VATOMIC_H

/*
 * libringbuffer/vatomic.h
 *
 * Copyright (C) 2010-2012 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <assert.h>
#include <urcu/uatomic.h>
#include <lttng/ust-rseq.h>
#include <cpu-op.h>

/*
 * Same data type (long) accessed differently depending on configuration.
 * v field is for non-atomic access (protected by mutual exclusion).
 * In the fast-path, the ring_buffer_config structure is constant, so the
 * compiler can statically select the appropriate branch.
 * local_t is used for per-cpu and per-thread buffers.
 * atomic_long_t is used for globally shared buffers.
 */
union v_atomic {
	long a;	/* accessed through uatomic */
	long v;
};

static inline
long v_read(const struct lttng_ust_lib_ring_buffer_config *config, union v_atomic *v_a)
{
	return uatomic_read(&v_a->a);
}

static inline
void v_set(const struct lttng_ust_lib_ring_buffer_config *config, union v_atomic *v_a,
	   long v)
{
	uatomic_set(&v_a->a, v);
}

static inline
void v_add(const struct lttng_ust_lib_ring_buffer_config *config, long v,
		union v_atomic *v_a, int cpu)
{
	if (caa_likely(config->sync == RING_BUFFER_SYNC_PER_CPU)) {
		int ret;

#ifndef SKIP_FASTPATH
		/* Try fast path. */
		ret = rseq_addv((intptr_t *)&v_a->a, v, cpu);
		if (caa_likely(!ret))
			return;
#endif
		for (;;) {
			/* Fallback on cpu_opv system call. */
			ret = cpu_op_addv((intptr_t *)&v_a->a, v, cpu);
			if (caa_likely(!ret))
				break;
			assert(ret >= 0 || errno == EAGAIN);
		}
	} else {
		goto atomic;
	}
	return;

atomic:
	uatomic_add(&v_a->a, v);
}

static inline
void v_inc(const struct lttng_ust_lib_ring_buffer_config *config,
		union v_atomic *v_a, int cpu)
{
	if (caa_likely(config->sync == RING_BUFFER_SYNC_PER_CPU)) {
		int ret;

#ifndef SKIP_FASTPATH
		/* Try fast path. */
		ret = rseq_addv((intptr_t *)&v_a->a, 1, cpu);
		if (caa_likely(!ret))
			return;
#endif
		for (;;) {
			/* Fallback on cpu_opv system call. */
			ret = cpu_op_addv((intptr_t *)&v_a->a, 1, cpu);
			if (caa_likely(!ret))
				break;
			assert(ret >= 0 || errno == EAGAIN);
		}
	} else {
		goto atomic;
	}
	return;

atomic:
	uatomic_inc(&v_a->a);
}

/*
 * Non-atomic decrement. Only used by reader, apply to reader-owned subbuffer.
 */
static inline
void _v_dec(const struct lttng_ust_lib_ring_buffer_config *config, union v_atomic *v_a)
{
	--v_a->v;
}

static inline
int v_cmpstore(const struct lttng_ust_lib_ring_buffer_config *config, union v_atomic *v_a,
	       long old, long _new, int cpu)
{
	if (caa_likely(config->sync == RING_BUFFER_SYNC_PER_CPU)) {
		int ret;

#ifndef SKIP_FASTPATH
		/* Try fast path. */
		ret = rseq_cmpeqv_storev((intptr_t *)&v_a->a, old, _new, cpu);
		if (caa_likely(!ret))
			return 0;
		if (caa_likely(ret > 0))
			return 1;
#endif
		/* Slow path. */
		for (;;) {
			/* Fallback on cpu_opv system call. */
			ret = cpu_op_cmpeqv_storev((intptr_t *)&v_a->a,
				old, _new, cpu);
			if (ret >= 0)
				break;
			assert(ret >= 0 || errno == EAGAIN);
		}
		return ret;
	} else {
		goto atomic;
	}

atomic:
	return uatomic_cmpxchg(&v_a->a, old, _new) != old;
}

#endif /* _LTTNG_RING_BUFFER_VATOMIC_H */
