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

#include <side/trace.h>

#include "lttng-bytecode.h"

struct side_event_description;
struct side_type;

/*
 * A filter names a payload field by a path: the session daemon emits a
 * chain of symbols as one dotted name, so "a.b" reaches the linker as
 * a single reloc symbol. The operand of a field reference is a 16-bit
 * index, which cannot hold a path, so the linker resolves the path
 * into the data area of the bytecode and the operand carries the
 * offset of that resolution instead.
 */
#define SIDE_FIELD_PATH_MAX	8

struct side_field_path {
	uint16_t nr;
	uint16_t idx[SIDE_FIELD_PATH_MAX];
};

/*
 * Resolve a dotted field name to a path of side argument indices, and
 * to the type of the field it reaches. Returns -1 when the name does
 * not name a field, or names one through something which is not a
 * structure.
 */
int lttng_bytecode_side_field_path(const struct side_event_description *side_desc,
		const char *name, struct side_field_path *path,
		const struct side_type **type)
	__attribute__((visibility("hidden")));

/*
 * Resolve the address a gather type reads its value from, and whether
 * a type is one.
 */
const char *side_gather_access(enum side_type_gather_access_mode access_mode,
		const char *ptr)
	__attribute__((visibility("hidden")));
bool side_arg_is_gather(const struct side_type *side_type)
	__attribute__((visibility("hidden")));

/* Load the integer a gather type reads from @gather_ptr. */
int side_gather_load_integer(const struct side_type_gather_integer *t,
		const void *gather_ptr, bool signedness, bool rev_bo, int64_t *v)
	__attribute__((visibility("hidden")));

/*
 * The elements of a gathered array or sequence: whether a type is such
 * a container, the type of its elements, and the address of the
 * element at @index, which is refused when the index is out of the
 * length of the container or when the elements are not of a fixed
 * size.
 */
bool side_type_is_struct(const struct side_type *side_type)
	__attribute__((visibility("hidden")));
const struct side_type *side_container_elem_type(const struct side_type *side_type)
	__attribute__((visibility("hidden")));
bool side_type_is_gather_container(const struct side_type *side_type)
	__attribute__((visibility("hidden")));
const struct side_type *side_gather_container_elem_type(const struct side_type *side_type)
	__attribute__((visibility("hidden")));
int side_gather_container_elem(const struct side_type *container,
		const void *value_base, const void *length_base,
		uint64_t index, const void **elem_base)
	__attribute__((visibility("hidden")));
void side_gather_container_arg_base(const struct side_type *container,
		const struct side_arg *item,
		const void **value_base, const void **length_base)
	__attribute__((visibility("hidden")));

/*
 * Descending into a structure: the type of the member at an index,
 * and, for a gathered structure, the address its members are read
 * from.
 */
const struct side_type *side_struct_member_type(const struct side_type *side_type,
		uint64_t idx)
	__attribute__((visibility("hidden")));
int side_struct_member_lookup_by_name(const struct side_type *side_type,
		const char *name, uint64_t *idx, const struct side_type **member_type)
	__attribute__((visibility("hidden")));
int side_gather_struct_base(const struct side_type *side_type,
		const void *base, const void **member_base)
	__attribute__((visibility("hidden")));

/*
 * The type and the value the @path of a payload field reaches, within
 * @sav. A gathered field reads its value from @gather_base rather than
 * from @arg, which is then NULL.
 */
struct side_field_ref {
	const struct side_arg *arg;
	const void *gather_base;
	const struct side_type *type;
};

/* Append to the data area of a linked bytecode. */
ssize_t lttng_bytecode_side_push_data(struct bytecode_runtime *runtime,
		const void *p, size_t align, size_t len)
	__attribute__((visibility("hidden")));

int lttng_bytecode_side_field_ref(const struct side_event_description *side_desc,
		const struct side_field_path *path,
		const struct side_arg_vec *sav,
		struct side_field_ref *ref)
	__attribute__((visibility("hidden")));

/*
 * Lookup a payload field by name in a side event description,
 * skipping no-filter fields the same way the tracer exposes them.
 * Returns the side argument index, and sets *type to the side type.
 * Returns -1 when not found.
 */
int lttng_bytecode_side_field_lookup(const struct side_event_description *side_desc,
		const char *name, const struct side_type **type)
	__attribute__((visibility("hidden")));

/*
 * Whether a value of this type must be converted from the byte order
 * it is emitted with to the byte order of the host before it is
 * compared.
 */
bool lttng_bytecode_side_type_rev_bo(const struct side_type *side_type)
	__attribute__((visibility("hidden")));

/*
 * The byte order of a payload field, by side argument index. Used by
 * the legacy field reference, whose operand is an index and which has
 * nowhere to carry the byte order the specialize phase resolves.
 */
bool lttng_bytecode_side_field_rev_bo(const struct side_event_description *side_desc,
		uint32_t idx)
	__attribute__((visibility("hidden")));

/*
 * The type of a payload field, by side argument index. A gather field
 * needs it: the argument holds the address its value is read from, and
 * only the type says how to reach it.
 */
const struct side_type *lttng_bytecode_side_field_type(
		const struct side_event_description *side_desc, uint32_t idx)
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
