/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2009-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (C) 2019 Michael Jeanson <mjeanson@efficios.com>
 *
 * LTTng UST ipc namespace context.
 */

#define _LGPL_SOURCE
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <lttng/ust-events.h>
#include <lttng/ust-tracer.h>
#include "common/compat/tid.h"
#include <urcu/tls-compat.h>
#include <lttng/ust-ringbuffer-context.h>

#include "context-internal.h"
#include "lttng-tracer-core.h"
#include "common/ns.h"

/*
 * We cache the result to ensure we don't stat(2) the proc filesystem on
 * each event.
 */
static DEFINE_URCU_TLS_INIT(ino_t, cached_ipc_ns, NS_INO_UNINITIALIZED);

static
ino_t get_ipc_ns(void)
{
	struct stat sb;
	ino_t ipc_ns;

	ipc_ns = CMM_LOAD_SHARED(URCU_TLS(cached_ipc_ns));

	/*
	 * If the cache is populated, do nothing and return the
	 * cached inode number.
	 */
	if (caa_likely(ipc_ns != NS_INO_UNINITIALIZED))
		return ipc_ns;

	/*
	 * At this point we have to populate the cache, set the initial
	 * value to NS_INO_UNAVAILABLE (0), if we fail to get the inode
	 * number from the proc filesystem, this is the value we will
	 * cache.
	 */
	ipc_ns = NS_INO_UNAVAILABLE;

	/*
	 * /proc/thread-self was introduced in kernel v3.17
	 */
	if (stat("/proc/thread-self/ns/ipc", &sb) == 0) {
		ipc_ns = sb.st_ino;
	} else {
		char proc_ns_path[LTTNG_PROC_NS_PATH_MAX];

		if (snprintf(proc_ns_path, LTTNG_PROC_NS_PATH_MAX,
				"/proc/self/task/%d/ns/ipc",
				lttng_gettid()) >= 0) {

			if (stat(proc_ns_path, &sb) == 0) {
				ipc_ns = sb.st_ino;
			}
		}
	}

	/*
	 * And finally, store the inode number in the cache.
	 */
	CMM_STORE_SHARED(URCU_TLS(cached_ipc_ns), ipc_ns);

	return ipc_ns;
}

/*
 * The ipc namespace can change for 3 reasons
 *  * clone(2) called with CLONE_NEWIPC
 *  * setns(2) called with the fd of a different ipc ns
 *  * unshare(2) called with CLONE_NEWIPC
 */
void lttng_context_ipc_ns_reset(void)
{
	CMM_STORE_SHARED(URCU_TLS(cached_ipc_ns), NS_INO_UNINITIALIZED);
}

static
size_t ipc_ns_get_size(void *priv __attribute__((unused)),
		struct lttng_ust_probe_ctx *probe_ctx __attribute__((unused)),
		size_t offset)
{
	size_t size = 0;

	size += lttng_ust_ring_buffer_align(offset, lttng_ust_rb_alignof(ino_t));
	size += sizeof(ino_t);
	return size;
}

static
void ipc_ns_record(void *priv __attribute__((unused)),
		struct lttng_ust_probe_ctx *probe_ctx __attribute__((unused)),
		struct lttng_ust_ring_buffer_ctx *ctx,
		struct lttng_ust_channel_buffer *chan)
{
	ino_t ipc_ns;

	ipc_ns = get_ipc_ns();
	chan->ops->event_write(ctx, &ipc_ns, sizeof(ipc_ns), lttng_ust_rb_alignof(ipc_ns));
}

static
void ipc_ns_get_value(void *priv __attribute__((unused)),
		struct lttng_ust_probe_ctx *probe_ctx __attribute__((unused)),
		struct lttng_ust_ctx_value *value)
{
	value->u.u64 = get_ipc_ns();
}

static const struct lttng_ust_ctx_field *ctx_field = lttng_ust_static_ctx_field(
	lttng_ust_static_event_field("ipc_ns",
		lttng_ust_static_type_integer(sizeof(ino_t) * CHAR_BIT,
				lttng_ust_rb_alignof(ino_t) * CHAR_BIT,
				lttng_ust_is_signed_type(ino_t),
				LTTNG_UST_BYTE_ORDER, 10),
		false, false),
	ipc_ns_get_size,
	ipc_ns_record,
	ipc_ns_get_value,
	NULL, NULL);

int lttng_add_ipc_ns_to_ctx(struct lttng_ust_ctx **ctx)
{
	int ret;

	if (lttng_find_context(*ctx, ctx_field->event_field->name)) {
		ret = -EEXIST;
		goto error_find_context;
	}
	ret = lttng_ust_context_append(ctx, ctx_field);
	if (ret)
		return ret;
	return 0;

error_find_context:
	return ret;
}

/*
 * Force a read (imply TLS allocation for dlopen) of TLS variables.
 */
void lttng_ipc_ns_alloc_tls(void)
{
	__asm__ __volatile__ ("" : : "m" (URCU_TLS(cached_ipc_ns)));
}
