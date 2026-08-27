/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * LTTng UST bytecode for side events: header.
 *
 * The side bytecode linker/specializer/interpreter reuse the LTTng
 * bytecode instruction set, validator and runtime structures, but
 * the interpreter input is the SIDE payload ABI (side_arg_vec)
 * instead of the marshalled interpreter stack data: payload field
 * references resolve to a side argument index at link/specialize
 * time, and field loads read the self-describing typed side
 * arguments directly.
 */

#ifndef _LTTNG_BYTECODE_SIDE_H
#define _LTTNG_BYTECODE_SIDE_H

#include "lttng-bytecode.h"

struct side_event_description;
struct side_type;

/*
 * Lookup a payload field by name in a side event description,
 * skipping no-filter fields the same way the tracer exposes them.
 * Returns the side argument index, and sets *type to the side type.
 * Returns -1 when not found.
 */
int lttng_bytecode_side_field_lookup(const struct side_event_description *side_desc,
		const char *name, const struct side_type **type)
	__attribute__((visibility("hidden")));

void lttng_enabler_link_bytecode_side(const struct lttng_ust_event_desc *event_desc,
		struct lttng_ust_ctx **ctx,
		struct cds_list_head *instance_bytecode_head,
		struct cds_list_head *enabler_bytecode_head)
	__attribute__((visibility("hidden")));

void lttng_bytecode_sync_state_side(struct lttng_ust_bytecode_runtime *runtime)
	__attribute__((visibility("hidden")));

int lttng_bytecode_specialize_side(const struct side_event_description *side_desc,
		struct bytecode_runtime *bytecode)
	__attribute__((visibility("hidden")));

int lttng_bytecode_interpret_side(struct lttng_ust_bytecode_runtime *bytecode_runtime,
		const char *interpreter_stack_data,
		struct lttng_ust_probe_ctx *probe_ctx,
		void *caller_ctx)
	__attribute__((visibility("hidden")));

#endif /* _LTTNG_BYTECODE_SIDE_H */
