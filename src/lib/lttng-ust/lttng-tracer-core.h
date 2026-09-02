/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2005-2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This contains the core definitions for the Linux Trace Toolkit.
 */

#ifndef _LTTNG_TRACER_CORE_H
#define _LTTNG_TRACER_CORE_H

#include <stddef.h>
#include <urcu/arch.h>
#include <urcu/list.h>
#include <lttng/ust-tracer.h>
#include <lttng/ust-ringbuffer-context.h>
#include "common/logging.h"

enum lttng_ust_init_thread_flags {
	LTTNG_UST_INIT_THREAD_CONTEXT_CACHE = (1 << 0),
	LTTNG_UST_INIT_THREAD_MASK = (LTTNG_UST_INIT_THREAD_CONTEXT_CACHE << 1) - 1,
};

struct lttng_ust_session;
struct lttng_ust_channel_buffer;
struct lttng_ust_ctx_field;
struct lttng_ust_ring_buffer_ctx;
struct lttng_ust_ctx_value;
struct lttng_ust_event_recorder;
struct lttng_ust_event_notifier;
struct lttng_ust_notification_ctx;

int ust_lock(void) __attribute__ ((warn_unused_result))
	__attribute__((visibility("hidden")));

void ust_lock_nocheck(void)
	__attribute__((visibility("hidden")));

void ust_unlock(void)
	__attribute__((visibility("hidden")));

void lttng_ust_common_init_thread(int flags)
	__attribute__((visibility("hidden")));

void lttng_ust_vtid_init_thread(int flags)
	__attribute__((visibility("hidden")));

void lttng_ust_procname_init_thread(int flags)
	__attribute__((visibility("hidden")));

void lttng_ust_cgroup_ns_init_thread(int flags)
	__attribute__((visibility("hidden")));

void lttng_ust_ipc_ns_init_thread(int flags)
	__attribute__((visibility("hidden")));

void lttng_ust_net_ns_init_thread(int flags)
	__attribute__((visibility("hidden")));

void lttng_ust_time_ns_init_thread(int flags)
	__attribute__((visibility("hidden")));

void lttng_ust_uts_ns_init_thread(int flags)
	__attribute__((visibility("hidden")));

const char *lttng_ust_obj_get_name(int id)
	__attribute__((visibility("hidden")));

int lttng_get_notify_socket(void *owner)
	__attribute__((visibility("hidden")));

char* lttng_ust_sockinfo_get_procname(void *owner)
	__attribute__((visibility("hidden")));

void lttng_ust_sockinfo_session_enabled(void *owner)
	__attribute__((visibility("hidden")));

void lttng_event_notifier_notification_send(
		const struct lttng_ust_event_notifier *event_notifier,
		const char *stack_data,
		struct lttng_ust_probe_ctx *probe_ctx,
		struct lttng_ust_notification_ctx *notif_ctx)
	__attribute__((visibility("hidden")));

void lttng_ust_side_tracer_init(void)
	__attribute__((visibility("hidden")));
void lttng_ust_side_tracer_exit(void)
	__attribute__((visibility("hidden")));

/*
 * Wait for a grace period of every domain from which the tracer can be
 * invoked: the LTTng-UST RCU domain, within which the tracepoint
 * probes are called, and the side domain, within which the side event
 * callbacks are called. Upon return, all events which had begun before
 * the call have completed, so data unpublished before the call can be
 * reclaimed.
 *
 * Must not be invoked from a probe.
 */
void lttng_ust_tracer_synchronize(void)
	__attribute__((visibility("hidden")));

/*
 * Recognition and connection of side-backed event descriptors.
 * Called under ust_mutex.
 */
struct lttng_ust_event_desc;
struct lttng_ust_event_common;
struct side_event_description;

const struct side_event_description *lttng_ust_side_get_side_desc(
		const struct lttng_ust_event_desc *desc)
	__attribute__((visibility("hidden")));
bool lttng_ust_side_is_side_event(const struct lttng_ust_event_desc *desc)
	__attribute__((visibility("hidden")));
int lttng_ust_side_register_event(const struct lttng_ust_event_desc *desc,
		struct lttng_ust_event_common *event)
	__attribute__((visibility("hidden")));
int lttng_ust_side_unregister_event(const struct lttng_ust_event_desc *desc,
		struct lttng_ust_event_common *event)
	__attribute__((visibility("hidden")));

/*
 * The registration and unregistration of the side event callbacks take
 * effect as they return, but their memory reclaim is deferred: it is
 * performed by this function, which waits for a single grace period of
 * the side event domain for the whole batch of events it is called
 * for. It is the counterpart of
 * lttng_ust_tp_probe_prune_release_queue() for the side events, and is
 * called at the same places.
 *
 * Called under ust_mutex.
 */
void lttng_ust_side_prune_release_queue(void)
	__attribute__((visibility("hidden")));

/*
 * Side statedump: a session identifies its side event callbacks by a
 * key, and requests the state of the application with it.
 */
struct lttng_ust_session;
int lttng_ust_side_session_key_alloc(struct lttng_ust_session *session)
	__attribute__((visibility("hidden")));
void lttng_ust_side_session_statedump(struct lttng_ust_session *session)
	__attribute__((visibility("hidden")));
void lttng_ust_side_session_statedump_cancel(struct lttng_ust_session *session)
	__attribute__((visibility("hidden")));

#ifdef HAVE_LINUX_PERF_EVENT_H
void lttng_ust_perf_counter_init_thread(int flags)
	__attribute__((visibility("hidden")));

void lttng_perf_lock(void)
	__attribute__((visibility("hidden")));

void lttng_perf_unlock(void)
	__attribute__((visibility("hidden")));
#else /* #ifdef HAVE_LINUX_PERF_EVENT_H */
static inline
void lttng_ust_perf_counter_init_thread(int flags __attribute__((unused)))
{
}
static inline
void lttng_perf_lock(void)
{
}
static inline
void lttng_perf_unlock(void)
{
}
#endif /* #else #ifdef HAVE_LINUX_PERF_EVENT_H */

#endif /* _LTTNG_TRACER_CORE_H */
