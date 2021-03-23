/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2019 (c) Francis Deslauriers <francis.deslauriers@efficios.com>
 */

#ifndef _LTTNG_UST_EVENTS_INTERNAL_H
#define _LTTNG_UST_EVENTS_INTERNAL_H

#include <stdint.h>

#include <urcu/list.h>
#include <urcu/hlist.h>

#include <helper.h>
#include <lttng/ust-events.h>

/*
 * Enabler field, within whatever object is enabling an event. Target of
 * backward reference.
 */
struct lttng_enabler {
	enum lttng_enabler_format_type format_type;

	/* head list of struct lttng_ust_filter_bytecode_node */
	struct cds_list_head filter_bytecode_head;
	/* head list of struct lttng_ust_excluder_node */
	struct cds_list_head excluder_head;

	struct lttng_ust_event event_param;
	unsigned int enabled:1;

	uint64_t user_token;		/* User-provided token */
};

struct lttng_event_enabler {
	struct lttng_enabler base;
	struct cds_list_head node;	/* per-session list of enablers */
	struct lttng_event_container *container;
	struct lttng_counter_key key;
	/*
	 * Unused, but kept around to make it explicit that the tracer can do
	 * it.
	 */
	struct lttng_ctx *ctx;
};

struct lttng_event_notifier_enabler {
	struct lttng_enabler base;
	uint64_t error_counter_index;
	struct cds_list_head node;	/* per-app list of event_notifier enablers */
	struct cds_list_head capture_bytecode_head;
	struct lttng_event_notifier_group *group; /* weak ref */
	uint64_t num_captures;
};

enum lttng_ust_bytecode_node_type {
	LTTNG_UST_BYTECODE_NODE_TYPE_FILTER,
	LTTNG_UST_BYTECODE_NODE_TYPE_CAPTURE,
};

struct lttng_ust_bytecode_node {
	enum lttng_ust_bytecode_node_type type;
	struct cds_list_head node;
	struct lttng_enabler *enabler;
	struct  {
		uint32_t len;
		uint32_t reloc_offset;
		uint64_t seqnum;
		char data[];
	} bc;
};

struct lttng_ust_excluder_node {
	struct cds_list_head node;
	struct lttng_enabler *enabler;
	/*
	 * struct lttng_ust_event_exclusion had variable sized array,
	 * must be last field.
	 */
	struct lttng_ust_event_exclusion excluder;
};

static inline
struct lttng_enabler *lttng_event_enabler_as_enabler(
		struct lttng_event_enabler *event_enabler)
{
	return &event_enabler->base;
}

static inline
struct lttng_enabler *lttng_event_notifier_enabler_as_enabler(
		struct lttng_event_notifier_enabler *event_notifier_enabler)
{
	return &event_notifier_enabler->base;
}

/*
 * Allocate and initialize a `struct lttng_event_enabler` object.
 *
 * On success, returns a `struct lttng_event_enabler`,
 * On memory error, returns NULL.
 */
LTTNG_HIDDEN
struct lttng_event_enabler *lttng_event_enabler_create(
		enum lttng_enabler_format_type format_type,
		struct lttng_ust_event *event_param,
		const struct lttng_ust_counter_key *key,
		struct lttng_event_container *container);

/*
 * Destroy a `struct lttng_event_enabler` object.
 */
LTTNG_HIDDEN
void lttng_event_enabler_destroy(struct lttng_event_enabler *enabler);

/*
 * Enable a `struct lttng_event_enabler` object and all events related to this
 * enabler.
 */
LTTNG_HIDDEN
int lttng_event_enabler_enable(struct lttng_event_enabler *enabler);

/*
 * Disable a `struct lttng_event_enabler` object and all events related to this
 * enabler.
 */
LTTNG_HIDDEN
int lttng_event_enabler_disable(struct lttng_event_enabler *enabler);

/*
 * Attach filter bytecode program to `struct lttng_event_enabler` and all
 * events related to this enabler.
 */
LTTNG_HIDDEN
int lttng_event_enabler_attach_filter_bytecode(
		struct lttng_event_enabler *enabler,
		struct lttng_ust_bytecode_node **bytecode);

/*
 * Attach an application context to an event enabler.
 *
 * Not implemented.
 */
LTTNG_HIDDEN
int lttng_event_enabler_attach_context(struct lttng_event_enabler *enabler,
		struct lttng_ust_context *ctx);

/*
 * Attach exclusion list to `struct lttng_event_enabler` and all
 * events related to this enabler.
 */
LTTNG_HIDDEN
int lttng_event_enabler_attach_exclusion(struct lttng_event_enabler *enabler,
		struct lttng_ust_excluder_node **excluder);

/*
 * Synchronize bytecodes for the enabler and the instance (event or
 * event_notifier).
 *
 * This function goes over all bytecode programs of the enabler (event or
 * event_notifier enabler) to ensure each is linked to the provided instance.
 */
LTTNG_HIDDEN
void lttng_enabler_link_bytecode(const struct lttng_event_desc *event_desc,
		struct lttng_ctx **ctx,
		struct cds_list_head *instance_bytecode_runtime_head,
		struct cds_list_head *enabler_bytecode_runtime_head);

/*
 * Allocate and initialize a `struct lttng_event_notifier_group` object.
 *
 * On success, returns a `struct lttng_triggre_group`,
 * on memory error, returns NULL.
 */
LTTNG_HIDDEN
struct lttng_event_notifier_group *lttng_event_notifier_group_create(void);

/*
 * Destroy a `struct lttng_event_notifier_group` object.
 */
LTTNG_HIDDEN
void lttng_event_notifier_group_destroy(
		struct lttng_event_notifier_group *event_notifier_group);

/*
 * Allocate and initialize a `struct lttng_event_notifier_enabler` object.
 *
 * On success, returns a `struct lttng_event_notifier_enabler`,
 * On memory error, returns NULL.
 */
LTTNG_HIDDEN
struct lttng_event_notifier_enabler *lttng_event_notifier_enabler_create(
		struct lttng_event_notifier_group *event_notifier_group,
		enum lttng_enabler_format_type format_type,
		struct lttng_ust_event_notifier *event_notifier_param);

/*
 * Destroy a `struct lttng_event_notifier_enabler` object.
 */
LTTNG_HIDDEN
void lttng_event_notifier_enabler_destroy(
		struct lttng_event_notifier_enabler *event_notifier_enabler);

/*
 * Enable a `struct lttng_event_notifier_enabler` object and all event
 * notifiers related to this enabler.
 */
LTTNG_HIDDEN
int lttng_event_notifier_enabler_enable(
		struct lttng_event_notifier_enabler *event_notifier_enabler);

/*
 * Disable a `struct lttng_event_notifier_enabler` object and all event
 * notifiers related to this enabler.
 */
LTTNG_HIDDEN
int lttng_event_notifier_enabler_disable(
		struct lttng_event_notifier_enabler *event_notifier_enabler);

/*
 * Attach filter bytecode program to `struct lttng_event_notifier_enabler` and
 * all event notifiers related to this enabler.
 */
LTTNG_HIDDEN
int lttng_event_notifier_enabler_attach_filter_bytecode(
		struct lttng_event_notifier_enabler *event_notifier_enabler,
		struct lttng_ust_bytecode_node **bytecode);

/*
 * Attach capture bytecode program to `struct lttng_event_notifier_enabler` and
 * all event_notifiers related to this enabler.
 */
LTTNG_HIDDEN
int lttng_event_notifier_enabler_attach_capture_bytecode(
		struct lttng_event_notifier_enabler *event_notifier_enabler,
		struct lttng_ust_bytecode_node **bytecode);

/*
 * Attach exclusion list to `struct lttng_event_notifier_enabler` and all
 * event notifiers related to this enabler.
 */
LTTNG_HIDDEN
int lttng_event_notifier_enabler_attach_exclusion(
		struct lttng_event_notifier_enabler *event_notifier_enabler,
		struct lttng_ust_excluder_node **excluder);

LTTNG_HIDDEN
void lttng_free_event_notifier_filter_runtime(
		struct lttng_event_notifier *event_notifier);

/*
 * Connect the probe on all enablers matching this event description.
 * Called on library load.
 */
LTTNG_HIDDEN
int lttng_fix_pending_event_notifiers(void);

LTTNG_HIDDEN
struct lttng_counter *lttng_ust_counter_create(
		const char *counter_transport_name,
		size_t number_dimensions,
		const size_t *max_nr_elem,
		int64_t global_sum_step,
		bool coalesce_hits);

LTTNG_HIDDEN
struct lttng_counter *lttng_session_create_counter(
	struct lttng_session *session,
	const char *counter_transport_name,
	size_t number_dimensions, const size_t *dimensions_sizes,
	int64_t global_sum_step, bool coalesce_hits);

#endif /* _LTTNG_UST_EVENTS_INTERNAL_H */
