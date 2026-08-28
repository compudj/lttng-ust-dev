/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2010-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * LTTng UST bytecode linker for side events.
 *
 * Adapted from lttng-bytecode.c: payload field references resolve to
 * a side argument index (stored in the field_ref offset / get_index
 * data) instead of an offset within the marshalled interpreter stack
 * data, since the interpreter input for side events is the SIDE
 * payload ABI (side_arg_vec) itself. Context references are handled
 * exactly like for tracepoint events.
 */

#define _LGPL_SOURCE
#include <stddef.h>
#include <stdint.h>

#include <urcu/rculist.h>

#include <side/trace.h>

#include "context-internal.h"
#include "lttng-bytecode.h"
#include "lttng-bytecode-side.h"
#include "lttng-tracer-core.h"
#include "lib/lttng-ust/events.h"
#include "common/macros.h"
#include "common/tracer.h"

/*
 * Whether loading a value of this type requires converting it from the
 * byte order it is emitted with to the byte order of the host.
 */
bool lttng_bytecode_side_type_rev_bo(const struct side_type *side_type)
{
	switch (side_enum_get(side_type->type)) {
	case SIDE_TYPE_U8:		/* Fall-through. */
	case SIDE_TYPE_S8:		/* Fall-through. */
	case SIDE_TYPE_BOOL:		/* Fall-through. */
	case SIDE_TYPE_BYTE:
		/* A single byte has no byte order. */
		return false;
	case SIDE_TYPE_U16:		/* Fall-through. */
	case SIDE_TYPE_U32:		/* Fall-through. */
	case SIDE_TYPE_U64:		/* Fall-through. */
	case SIDE_TYPE_S16:		/* Fall-through. */
	case SIDE_TYPE_S32:		/* Fall-through. */
	case SIDE_TYPE_S64:		/* Fall-through. */
	case SIDE_TYPE_POINTER:
		return side_enum_get(side_type->u.side_integer.byte_order) !=
			SIDE_TYPE_BYTE_ORDER_HOST;
	case SIDE_TYPE_FLOAT_BINARY32:	/* Fall-through. */
	case SIDE_TYPE_FLOAT_BINARY64:
		return side_enum_get(side_type->u.side_float.byte_order) !=
			SIDE_TYPE_FLOAT_WORD_ORDER_HOST;
	case SIDE_TYPE_ENUM:
	{
		const struct side_type *container =
			side_ptr_get(side_type->u.side_enum.elem_type);

		return lttng_bytecode_side_type_rev_bo(container);
	}
	default:
		return false;
	}
}

bool lttng_bytecode_side_field_rev_bo(const struct side_event_description *side_desc,
		uint32_t idx)
{
	const struct side_event_field *field;

	if (!side_desc || idx >= side_array_length(&side_desc->fields))
		return false;
	field = side_array_at(&side_desc->fields, idx);
	return lttng_bytecode_side_type_rev_bo(&field->side_type);
}

int lttng_bytecode_side_field_lookup(const struct side_event_description *side_desc,
		const char *name, const struct side_type **type)
{
	uint32_t i, nr_fields;

	nr_fields = side_array_length(&side_desc->fields);
	for (i = 0; i < nr_fields; i++) {
		const struct side_event_field *field =
			side_array_at(&side_desc->fields, i);

		if (!strcmp(side_ptr_get(field->field_name), name)) {
			*type = &field->side_type;
			return (int) i;
		}
	}
	return -1;
}

static
int apply_field_reloc_side(const struct lttng_ust_event_desc *event_desc,
		struct bytecode_runtime *runtime,
		uint32_t runtime_len __attribute__((unused)),
		uint32_t reloc_offset,
		const char *field_name,
		enum bytecode_op bytecode_op)
{
	const struct side_event_description *side_desc;
	const struct side_type *side_type;
	struct load_op *op;
	int idx;

	dbg_printf("Apply side field reloc: %u %s\n", reloc_offset, field_name);

	if (!event_desc)
		return -EINVAL;
	side_desc = lttng_ust_side_get_side_desc(event_desc);
	if (!side_desc)
		return -EINVAL;
	idx = lttng_bytecode_side_field_lookup(side_desc, field_name, &side_type);
	if (idx < 0)
		return -EINVAL;

	/* Check if index is too large for 16-bit offset */
	if (idx > LTTNG_UST_ABI_FILTER_BYTECODE_MAX_LEN - 1)
		return -EINVAL;

	/* set type */
	op = (struct load_op *) &runtime->code[reloc_offset];

	switch (bytecode_op) {
	case BYTECODE_OP_LOAD_FIELD_REF:
	{
		struct field_ref *field_ref;

		field_ref = (struct field_ref *) op->data;
		switch (side_enum_get(side_type->type)) {
		case SIDE_TYPE_U8:		/* Fall-through. */
		case SIDE_TYPE_U16:		/* Fall-through. */
		case SIDE_TYPE_U32:		/* Fall-through. */
		case SIDE_TYPE_U64:		/* Fall-through. */
		case SIDE_TYPE_S8:		/* Fall-through. */
		case SIDE_TYPE_S16:		/* Fall-through. */
		case SIDE_TYPE_S32:		/* Fall-through. */
		case SIDE_TYPE_S64:		/* Fall-through. */
		case SIDE_TYPE_BOOL:		/* Fall-through. */
		case SIDE_TYPE_BYTE:		/* Fall-through. */
		case SIDE_TYPE_POINTER:
			op->op = BYTECODE_OP_LOAD_FIELD_REF_S64;
			break;
		case SIDE_TYPE_FLOAT_BINARY32:	/* Fall-through. */
		case SIDE_TYPE_FLOAT_BINARY64:
			op->op = BYTECODE_OP_LOAD_FIELD_REF_DOUBLE;
			break;
		case SIDE_TYPE_STRING_UTF8:
			op->op = BYTECODE_OP_LOAD_FIELD_REF_STRING;
			break;
		case SIDE_TYPE_ENUM:
			/*
			 * An enumeration compares as the integer value
			 * of its container: the argument of an
			 * enumeration field is that integer.
			 */
			op->op = BYTECODE_OP_LOAD_FIELD_REF_S64;
			break;
		default:
			/*
			 * Compound types cannot be loaded as a value.
			 * Arrays and VLAs are only reachable through
			 * element indexing, which the filter compiler
			 * emits as a symbol lookup handled by the
			 * specialize phase.
			 */
			return -EINVAL;
		}
		/* set side argument index */
		field_ref->offset = (uint16_t) idx;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

/* Identical to the tracepoint flavor: contexts are tracer-side. */
static
int apply_context_reloc_side(struct bytecode_runtime *runtime,
		uint32_t runtime_len __attribute__((unused)),
		uint32_t reloc_offset,
		const char *context_name,
		enum bytecode_op bytecode_op)
{
	struct load_op *op;
	const struct lttng_ust_ctx_field *ctx_field;
	int idx;
	struct lttng_ust_ctx **pctx = runtime->p.pctx;

	dbg_printf("Apply context reloc: %u %s\n", reloc_offset, context_name);

	/* Get context index */
	idx = lttng_get_context_index(*pctx, context_name);
	if (idx < 0) {
		if (lttng_context_is_app(context_name)) {
			int ret;

			ret = lttng_ust_add_app_context_to_ctx_rcu(context_name,
					pctx);
			if (ret)
				return ret;
			idx = lttng_get_context_index(*pctx, context_name);
			if (idx < 0)
				return -ENOENT;
		} else {
			return -ENOENT;
		}
	}
	/* Check if idx is too large for 16-bit offset */
	if (idx > LTTNG_UST_ABI_FILTER_BYTECODE_MAX_LEN - 1)
		return -EINVAL;

	/* Get context return type */
	ctx_field = &(*pctx)->fields[idx];
	op = (struct load_op *) &runtime->code[reloc_offset];

	switch (bytecode_op) {
	case BYTECODE_OP_GET_CONTEXT_REF:
	{
		struct field_ref *field_ref;

		field_ref = (struct field_ref *) op->data;
		switch (ctx_field->event_field->type->type) {
		case lttng_ust_type_integer:
		case lttng_ust_type_enum:
			op->op = BYTECODE_OP_GET_CONTEXT_REF_S64;
			break;
			/* Sequence and array supported only as string */
		case lttng_ust_type_array:
		{
			struct lttng_ust_type_array *array = (struct lttng_ust_type_array *) ctx_field->event_field->type;

			if (array->encoding == lttng_ust_string_encoding_none)
				return -EINVAL;
			op->op = BYTECODE_OP_GET_CONTEXT_REF_STRING;
			break;
		}
		case lttng_ust_type_sequence:
		{
			struct lttng_ust_type_sequence *sequence = (struct lttng_ust_type_sequence *) ctx_field->event_field->type;

			if (sequence->encoding == lttng_ust_string_encoding_none)
				return -EINVAL;
			op->op = BYTECODE_OP_GET_CONTEXT_REF_STRING;
			break;
		}
		case lttng_ust_type_string:
			op->op = BYTECODE_OP_GET_CONTEXT_REF_STRING;
			break;
		case lttng_ust_type_float:
			op->op = BYTECODE_OP_GET_CONTEXT_REF_DOUBLE;
			break;
		case lttng_ust_type_dynamic:
			op->op = BYTECODE_OP_GET_CONTEXT_REF;
			break;
			/* Blobs are not supported as strings. */
		case lttng_ust_type_fixed_length_blob:		/* Fall-through. */
		case lttng_ust_type_variable_length_blob:
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
		/* set offset to context index within channel contexts */
		field_ref->offset = (uint16_t) idx;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

static
int apply_reloc_side(const struct lttng_ust_event_desc *event_desc,
		struct bytecode_runtime *runtime,
		uint32_t runtime_len,
		uint32_t reloc_offset,
		const char *name)
{
	struct load_op *op;

	dbg_printf("Apply side reloc: %u %s\n", reloc_offset, name);

	/* Ensure that the reloc is within the code */
	if (runtime_len - reloc_offset < sizeof(uint16_t))
		return -EINVAL;

	op = (struct load_op *) &runtime->code[reloc_offset];
	switch (op->op) {
	case BYTECODE_OP_LOAD_FIELD_REF:
		return apply_field_reloc_side(event_desc, runtime, runtime_len,
			reloc_offset, name, op->op);
	case BYTECODE_OP_GET_CONTEXT_REF:
		return apply_context_reloc_side(runtime, runtime_len,
			reloc_offset, name, op->op);
	case BYTECODE_OP_GET_SYMBOL:
	case BYTECODE_OP_GET_SYMBOL_FIELD:
		/*
		 * Will be handled by load specialize phase or
		 * dynamically by interpreter.
		 */
		return 0;
	default:
		ERR("Unknown reloc op type %u\n", op->op);
		return -EINVAL;
	}
	return 0;
}

static
int bytecode_is_linked(struct lttng_ust_bytecode_node *bytecode,
		struct cds_list_head *bytecode_runtime_head)
{
	struct lttng_ust_bytecode_runtime *bc_runtime;

	cds_list_for_each_entry(bc_runtime, bytecode_runtime_head, node) {
		if (bc_runtime->bc == bytecode)
			return 1;
	}
	return 0;
}

/*
 * Take a bytecode with reloc table and link it to a side event to
 * create a bytecode runtime.
 */
static
int link_bytecode_side(const struct lttng_ust_event_desc *event_desc,
		struct lttng_ust_ctx **ctx,
		struct lttng_ust_bytecode_node *bytecode,
		struct cds_list_head *bytecode_runtime_head,
		struct cds_list_head *insert_loc)
{
	int ret, offset, next_offset;
	struct bytecode_runtime *runtime = NULL;
	size_t runtime_alloc_len;
	const struct side_event_description *side_desc;

	if (!bytecode)
		return 0;
	/* Bytecode already linked */
	if (bytecode_is_linked(bytecode, bytecode_runtime_head))
		return 0;

	side_desc = lttng_ust_side_get_side_desc(event_desc);
	if (!side_desc)
		return -EINVAL;

	dbg_printf("Linking (side)...\n");

	/* We don't need the reloc table in the runtime */
	runtime_alloc_len = sizeof(*runtime) + bytecode->bc.reloc_offset;
	runtime = zmalloc(runtime_alloc_len);
	if (!runtime) {
		ret = -ENOMEM;
		goto alloc_error;
	}
	runtime->p.type = bytecode->type;
	runtime->p.bc = bytecode;
	runtime->p.pctx = ctx;
	/* The interpreter needs the field types this is linked against. */
	runtime->side_desc = side_desc;
	runtime->len = bytecode->bc.reloc_offset;
	/* copy original bytecode */
	memcpy(runtime->code, bytecode->bc.data, runtime->len);
	/* Validate bytecode load instructions before relocs. */
	ret = lttng_bytecode_validate_load(runtime);
	if (ret) {
		goto link_error;
	}
	/*
	 * apply relocs. Those are a uint16_t (offset in bytecode)
	 * followed by a string (field name).
	 */
	for (offset = bytecode->bc.reloc_offset;
			offset < bytecode->bc.len;
			offset = next_offset) {
		uint16_t reloc_offset =
			*(uint16_t *) &bytecode->bc.data[offset];
		const char *name =
			(const char *) &bytecode->bc.data[offset + sizeof(uint16_t)];

		ret = apply_reloc_side(event_desc, runtime, runtime->len, reloc_offset, name);
		if (ret) {
			goto link_error;
		}
		next_offset = offset + sizeof(uint16_t) + strlen(name) + 1;
	}
	/* Validate bytecode */
	ret = lttng_bytecode_validate(runtime);
	if (ret) {
		goto link_error;
	}
	/* Specialize bytecode */
	ret = lttng_bytecode_specialize_side(side_desc, runtime);
	if (ret) {
		goto link_error;
	}

	runtime->p.interpreter_func = lttng_bytecode_interpret_side;
	runtime->p.link_failed = 0;
	cds_list_add_rcu(&runtime->p.node, insert_loc);
	dbg_printf("Linking successful (side).\n");
	return 0;

link_error:
	runtime->p.interpreter_func = lttng_bytecode_interpret_error;
	runtime->p.link_failed = 1;
	cds_list_add_rcu(&runtime->p.node, insert_loc);
alloc_error:
	dbg_printf("Linking failed (side).\n");
	return ret;
}

void lttng_bytecode_sync_state_side(struct lttng_ust_bytecode_runtime *runtime)
{
	struct lttng_ust_bytecode_node *bc = runtime->bc;

	if (!bc->enabler->enabled || runtime->link_failed)
		runtime->interpreter_func = lttng_bytecode_interpret_error;
	else
		runtime->interpreter_func = lttng_bytecode_interpret_side;
}

/*
 * Given the lists of bytecode programs of an instance (trigger or
 * event) and of a matching enabler, try to link all the enabler's
 * bytecode programs with the side event instance.
 */
void lttng_enabler_link_bytecode_side(const struct lttng_ust_event_desc *event_desc,
		struct lttng_ust_ctx **ctx,
		struct cds_list_head *instance_bytecode_head,
		struct cds_list_head *enabler_bytecode_head)
{
	struct lttng_ust_bytecode_node *enabler_bc;
	struct lttng_ust_bytecode_runtime *runtime;

	assert(event_desc);

	/* Go over all the bytecode programs of the enabler. */
	cds_list_for_each_entry(enabler_bc, enabler_bytecode_head, node) {
		int found = 0, ret;
		struct cds_list_head *insert_loc;

		/*
		 * Check if the current enabler bytecode program is already
		 * linked with the instance.
		 */
		cds_list_for_each_entry(runtime, instance_bytecode_head, node) {
			if (runtime->bc == enabler_bc) {
				found = 1;
				break;
			}
		}

		/*
		 * Skip bytecode already linked, go to the next enabler
		 * bytecode program.
		 */
		if (found)
			continue;

		/*
		 * Insert at specified priority (seqnum) in increasing
		 * order. If there already is a bytecode of the same priority,
		 * insert the new bytecode right after it.
		 */
		cds_list_for_each_entry_reverse(runtime,
				instance_bytecode_head, node) {
			if (runtime->bc->bc.seqnum <= enabler_bc->bc.seqnum) {
				/* insert here */
				insert_loc = &runtime->node;
				goto add_within;
			}
		}

		/* Add to head to list */
		insert_loc = instance_bytecode_head;
	add_within:
		dbg_printf("linking side bytecode\n");
		ret = link_bytecode_side(event_desc, ctx, enabler_bc,
			instance_bytecode_head, insert_loc);
		if (ret) {
			dbg_printf("[lttng filter] warning: cannot link side event bytecode\n");
		}
	}
}
