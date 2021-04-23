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

void lttng_ust_alloc_tls(void)
	__attribute__((visibility("hidden")));

void lttng_vtid_alloc_tls(void)
	__attribute__((visibility("hidden")));

void lttng_procname_alloc_tls(void)
	__attribute__((visibility("hidden")));

void lttng_cgroup_ns_alloc_tls(void)
	__attribute__((visibility("hidden")));

void lttng_ipc_ns_alloc_tls(void)
	__attribute__((visibility("hidden")));

void lttng_net_ns_alloc_tls(void)
	__attribute__((visibility("hidden")));

void lttng_time_ns_alloc_tls(void)
	__attribute__((visibility("hidden")));

void lttng_uts_ns_alloc_tls(void)
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

#ifdef HAVE_LINUX_PERF_EVENT_H
void lttng_ust_perf_counter_alloc_tls(void)
	__attribute__((visibility("hidden")));

void lttng_perf_lock(void)
	__attribute__((visibility("hidden")));

void lttng_perf_unlock(void)
	__attribute__((visibility("hidden")));
#else /* #ifdef HAVE_LINUX_PERF_EVENT_H */
static inline
void lttng_ust_perf_counter_alloc_tls(void)
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
