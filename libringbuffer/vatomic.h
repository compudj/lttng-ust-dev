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
#include <lttng/rseq.h>

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
#ifndef SKIP_FASTPATH
		struct rseq_state rseq_state;
		intptr_t *targetptr, newval;

		/* Try fast path. */
		rseq_state = rseq_start();
		if (caa_unlikely(rseq_cpu_at_start(rseq_state) < 0))
			goto atomic;
		if (caa_unlikely(rseq_cpu_at_start(rseq_state) != cpu))
			goto slowpath;
		newval = (intptr_t)v_a->a + v;
		targetptr = (intptr_t *)&v_a->a;
		if (unlikely(!rseq_finish(targetptr, newval, rseq_state)))
			goto slowpath;
		return;
#endif
slowpath:
		for (;;) {
			/* Fallback on rseq_op system call. */
			int ret;

			ret = rseq_op_add(&v_a->a, v,
				sizeof(v_a->a), cpu);
			if (likely(!ret))
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
#ifndef SKIP_FASTPATH
		struct rseq_state rseq_state;
		intptr_t *targetptr, newval;

		/* Try fast path. */
		rseq_state = rseq_start();
		if (caa_unlikely(rseq_cpu_at_start(rseq_state) < 0))
			goto atomic;
		if (caa_unlikely(rseq_cpu_at_start(rseq_state) != cpu))
			goto slowpath;
		newval = (intptr_t)v_a->a + 1;
		targetptr = (intptr_t *)&v_a->a;
		if (unlikely(!rseq_finish(targetptr, newval, rseq_state)))
			goto slowpath;
		return;
#endif
slowpath:
		for (;;) {
			/* Fallback on rseq_op system call. */
			int ret;

			ret = rseq_op_add(&v_a->a, 1,
				sizeof(v_a->a), cpu);
			if (likely(!ret))
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
long v_cmpxchg(const struct lttng_ust_lib_ring_buffer_config *config, union v_atomic *v_a,
	       long old, long _new, int cpu)
{
	if (caa_likely(config->sync == RING_BUFFER_SYNC_PER_CPU)) {
#ifndef SKIP_FASTPATH
		struct rseq_state rseq_state;
		intptr_t *targetptr, newval, resval, oldval;

		/* Try fast path. */
		rseq_state = rseq_start();
		if (caa_unlikely(rseq_cpu_at_start(rseq_state) < 0))
			goto atomic;
		if (caa_unlikely(rseq_cpu_at_start(rseq_state) != cpu))
			goto slowpath;
		oldval = (intptr_t)old;
		resval = (intptr_t)v_read(config, v_a);
		if (resval != oldval)
			goto end;
		newval = (intptr_t)_new;
		targetptr = (intptr_t *)&v_a->a;
		if (unlikely(!rseq_finish(targetptr, newval, rseq_state)))
			goto slowpath;
end:
		return (long)resval;
#endif
slowpath:
		{
			long res;

			for (;;) {
				/* Fallback on rseq_op system call. */
				int ret;

				ret = rseq_op_cmpxchg(&v_a->a, &old,
					&res, &_new, sizeof(v_a->a), cpu);
				if (ret >= 0)
					break;
				assert(ret >= 0 || errno == EAGAIN);
			}
			return (long)res;
		}
	} else {
		goto atomic;
	}

atomic:

	return uatomic_cmpxchg(&v_a->a, old, _new);
}

#endif /* _LTTNG_RING_BUFFER_VATOMIC_H */
