/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2009-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (C) 2019 Michael Jeanson <mjeanson@efficios.com>
 *
 * LTTng UST namespaced real group ID context.
 */

#define _LGPL_SOURCE
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <lttng/ust-events.h>
#include <lttng/ust-tracer.h>
#include <lttng/ust-ringbuffer-context.h>

#include "context-internal.h"
#include "common/creds.h"


/*
 * At the kernel level, user IDs and group IDs are a per-thread attribute.
 * However, POSIX requires that all threads in a process share the same
 * credentials. The NPTL threading implementation handles the POSIX
 * requirements by providing wrapper functions for the various system calls
 * that change process UIDs and GIDs. These wrapper functions (including those
 * for setreuid() and setregid()) employ a signal-based technique to ensure
 * that when one thread changes credentials, all of the other threads in the
 * process also change their credentials.
 */

/*
 * We cache the result to ensure we don't trigger a system call for
 * each event. User / group IDs are global to the process.
 */
static gid_t cached_vgid = INVALID_GID;

static
gid_t get_vgid(void)
{
	gid_t vgid;

	vgid = CMM_LOAD_SHARED(cached_vgid);

	if (caa_unlikely(cached_vgid == (gid_t) -1)) {
		vgid = getgid();
		CMM_STORE_SHARED(cached_vgid, vgid);
	}

	return vgid;
}

/*
 * The vgid can change on setuid, setreuid and setresuid.
 */
void lttng_context_vgid_reset(void)
{
	CMM_STORE_SHARED(cached_vgid, INVALID_GID);
}

static
size_t vgid_get_size(void *priv __attribute__((unused)),
		struct lttng_ust_probe_ctx *probe_ctx __attribute__((unused)),
		size_t offset)
{
	size_t size = 0;

	size += lttng_ust_ring_buffer_align(offset, lttng_ust_rb_alignof(gid_t));
	size += sizeof(gid_t);
	return size;
}

static
void vgid_record(void *priv __attribute__((unused)),
		struct lttng_ust_probe_ctx *probe_ctx __attribute__((unused)),
		struct lttng_ust_ring_buffer_ctx *ctx,
		struct lttng_ust_channel_buffer *chan)
{
	gid_t vgid;

	vgid = get_vgid();
	chan->ops->event_write(ctx, &vgid, sizeof(vgid), lttng_ust_rb_alignof(vgid));
}

static
void vgid_get_value(void *priv __attribute__((unused)),
		struct lttng_ust_probe_ctx *probe_ctx __attribute__((unused)),
		struct lttng_ust_ctx_value *value)
{
	value->u.u64 = get_vgid();
}

static const struct lttng_ust_ctx_field *ctx_field = lttng_ust_static_ctx_field(
	lttng_ust_static_event_field("vgid",
		lttng_ust_static_type_integer(sizeof(gid_t) * CHAR_BIT,
				lttng_ust_rb_alignof(gid_t) * CHAR_BIT,
				lttng_ust_is_signed_type(gid_t),
				LTTNG_UST_BYTE_ORDER, 10),
		false, false),
	vgid_get_size,
	vgid_record,
	vgid_get_value,
	NULL, NULL);

int lttng_add_vgid_to_ctx(struct lttng_ust_ctx **ctx)
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
