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
			side_ptr_rel_get(side_type->u.side_enum.elem_type);

		return lttng_bytecode_side_type_rev_bo(container);
	}
	/* A gathered value has the byte order of the type it wraps. */
	case SIDE_TYPE_GATHER_BOOL:	/* Fall-through. */
	case SIDE_TYPE_GATHER_BYTE:
		return false;
	case SIDE_TYPE_GATHER_INTEGER:	/* Fall-through. */
	case SIDE_TYPE_GATHER_POINTER:
		return side_enum_get(side_type->u.side_gather.u.side_integer.type.byte_order) !=
			SIDE_TYPE_BYTE_ORDER_HOST;
	case SIDE_TYPE_GATHER_FLOAT:
		return side_enum_get(side_type->u.side_gather.u.side_float.type.byte_order) !=
			SIDE_TYPE_FLOAT_WORD_ORDER_HOST;
	case SIDE_TYPE_GATHER_ENUM:
		return lttng_bytecode_side_type_rev_bo(side_ptr_rel_get(
			side_type->u.side_gather.u.side_enum.elem_type));
	default:
		return false;
	}
}

const struct side_type *lttng_bytecode_side_field_type(
		const struct side_event_description *side_desc, uint32_t idx)
{
	const struct side_event_field *field;

	if (!side_desc || idx >= side_array_length(&side_desc->fields))
		return NULL;
	field = side_array_rel_at(&side_desc->fields, idx);
	return &field->side_type;
}

bool lttng_bytecode_side_field_rev_bo(const struct side_event_description *side_desc,
		uint32_t idx)
{
	const struct side_event_field *field;

	if (!side_desc || idx >= side_array_length(&side_desc->fields))
		return false;
	field = side_array_rel_at(&side_desc->fields, idx);
	return lttng_bytecode_side_type_rev_bo(&field->side_type);
}

/*
 * A gather type reads its value from an address rather than carrying
 * it in the argument, which holds the address instead. The access mode
 * says whether that address is the value's own or a pointer to it.
 */
const char *side_gather_access(enum side_type_gather_access_mode access_mode,
		const char *ptr)
{
	switch (access_mode) {
	case SIDE_TYPE_GATHER_ACCESS_DIRECT:
		return ptr;
	case SIDE_TYPE_GATHER_ACCESS_POINTER:
		/* Dereference pointer */
		memcpy(&ptr, ptr, sizeof(const char *));
		return ptr;
	default:
		return NULL;
	}
}

bool side_arg_is_gather(const struct side_type *side_type)
{
	switch (side_enum_get(side_type->type)) {
	case SIDE_TYPE_GATHER_BOOL:	/* Fall-through. */
	case SIDE_TYPE_GATHER_BYTE:	/* Fall-through. */
	case SIDE_TYPE_GATHER_INTEGER:	/* Fall-through. */
	case SIDE_TYPE_GATHER_POINTER:	/* Fall-through. */
	case SIDE_TYPE_GATHER_FLOAT:	/* Fall-through. */
	case SIDE_TYPE_GATHER_STRING:	/* Fall-through. */
	case SIDE_TYPE_GATHER_ENUM:	/* Fall-through. */
	case SIDE_TYPE_GATHER_STRUCT:	/* Fall-through. */
	case SIDE_TYPE_GATHER_ARRAY:	/* Fall-through. */
	case SIDE_TYPE_GATHER_VLA:
		return true;
	default:
		return false;
	}
}

/*
 * Load the integer a gather type reads from memory. Bit-packed values
 * are refused, as they are by the translation of the event.
 */
int side_gather_load_integer(const struct side_type_gather_integer *t,
		const void *gather_ptr, bool signedness, bool rev_bo, int64_t *v)
{
	union side_integer_value value;
	const char *ptr;

	if (t->offset_bits)
		return -EINVAL;
	ptr = side_gather_access(side_enum_get(t->access_mode),
			(const char *) gather_ptr + t->offset);
	if (!ptr)
		return -EINVAL;
	switch (t->type.integer_size) {
	case 1:		/* Fall-through. */
	case 2:		/* Fall-through. */
	case 4:		/* Fall-through. */
	case 8:
		break;
	default:
		return -EINVAL;
	}
	memcpy(&value, ptr, t->type.integer_size);
	switch (t->type.integer_size) {
	case 1:
		*v = signedness ? (int64_t) (int8_t) value.side_u8 :
				(int64_t) value.side_u8;
		break;
	case 2:
	{
		uint16_t tmp = value.side_u16;

		if (rev_bo)
			tmp = lttng_ust_bswap_16(tmp);
		*v = signedness ? (int64_t) (int16_t) tmp : (int64_t) tmp;
		break;
	}
	case 4:
	{
		uint32_t tmp = value.side_u32;

		if (rev_bo)
			tmp = lttng_ust_bswap_32(tmp);
		*v = signedness ? (int64_t) (int32_t) tmp : (int64_t) tmp;
		break;
	}
	case 8:
	{
		uint64_t tmp = value.side_u64;

		if (rev_bo)
			tmp = lttng_ust_bswap_64(tmp);
		*v = (int64_t) tmp;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * The fields of a structure, whether it is copied onto the argument
 * vector or gathered from memory. NULL for anything else.
 */
static
const struct side_type_struct *side_type_struct_fields(const struct side_type *side_type)
{
	switch (side_enum_get(side_type->type)) {
	case SIDE_TYPE_STRUCT:
		return side_ptr_sel_get(side_type->u.side_struct);
	case SIDE_TYPE_GATHER_STRUCT:
		return side_ptr_sel_get(side_type->u.side_gather.u.side_struct.type);
	default:
		return NULL;
	}
}

/* The element type of an array or a sequence copied onto the arguments. */
bool side_type_is_struct(const struct side_type *side_type)
{
	return side_type && side_type_struct_fields(side_type) != NULL;
}

const struct side_type *side_container_elem_type(const struct side_type *side_type)
{
	if (!side_type)
		return NULL;
	switch (side_enum_get(side_type->type)) {
	case SIDE_TYPE_ARRAY:
		return side_ptr_rel_get(side_ptr_sel_get(side_type->u.side_array)->elem_type);
	case SIDE_TYPE_VLA:
		return side_ptr_rel_get(side_ptr_sel_get(side_type->u.side_vla)->elem_type);
	default:
		return NULL;
	}
}

bool side_type_is_gather_container(const struct side_type *side_type)
{
	switch (side_enum_get(side_type->type)) {
	case SIDE_TYPE_GATHER_ARRAY:	/* Fall-through. */
	case SIDE_TYPE_GATHER_VLA:
		return true;
	default:
		return false;
	}
}

const struct side_type *side_gather_container_elem_type(const struct side_type *side_type)
{
	switch (side_enum_get(side_type->type)) {
	case SIDE_TYPE_GATHER_ARRAY:
		return side_ptr_rel_get(side_type->u.side_gather.u.side_array.type.elem_type);
	case SIDE_TYPE_GATHER_VLA:
		return side_ptr_rel_get(side_type->u.side_gather.u.side_vla.type.elem_type);
	default:
		return NULL;
	}
}

/*
 * How far apart the elements of a gathered container are: the size the
 * element occupies where it is read from, which is the size of a
 * pointer when the element is reached through one. Zero when the
 * element is not of a fixed size, which makes the elements which
 * follow it unreachable.
 */
static
uint32_t side_gather_elem_stride(const struct side_type *elem_type)
{
	const struct side_type_gather *gather = &elem_type->u.side_gather;
	enum side_type_gather_access_mode access_mode;
	uint32_t size;

	switch (side_enum_get(elem_type->type)) {
	case SIDE_TYPE_GATHER_BOOL:
		access_mode = side_enum_get(gather->u.side_bool.access_mode);
		size = gather->u.side_bool.type.bool_size;
		break;
	case SIDE_TYPE_GATHER_BYTE:
		access_mode = side_enum_get(gather->u.side_byte.access_mode);
		size = 1;
		break;
	case SIDE_TYPE_GATHER_INTEGER:	/* Fall-through. */
	case SIDE_TYPE_GATHER_POINTER:
		access_mode = side_enum_get(gather->u.side_integer.access_mode);
		size = gather->u.side_integer.type.integer_size;
		break;
	case SIDE_TYPE_GATHER_FLOAT:
		access_mode = side_enum_get(gather->u.side_float.access_mode);
		size = gather->u.side_float.type.float_size;
		break;
	case SIDE_TYPE_GATHER_STRUCT:
		access_mode = side_enum_get(gather->u.side_struct.access_mode);
		size = gather->u.side_struct.size;
		break;
	case SIDE_TYPE_GATHER_ENUM:
	{
		const struct side_type *container =
			side_ptr_rel_get(gather->u.side_enum.elem_type);

		if (side_enum_get(container->type) != SIDE_TYPE_GATHER_INTEGER)
			return 0;
		access_mode = side_enum_get(container->u.side_gather.u.side_integer.access_mode);
		size = container->u.side_gather.u.side_integer.type.integer_size;
		break;
	}
	default:
		/* A gathered string has no size of its own. */
		return 0;
	}
	switch (access_mode) {
	case SIDE_TYPE_GATHER_ACCESS_DIRECT:
		return size;
	case SIDE_TYPE_GATHER_ACCESS_POINTER:
		return sizeof(void *);
	default:
		return 0;
	}
}

int side_gather_container_elem(const struct side_type *container,
		const void *value_base, const void *length_base,
		uint64_t index, const void **elem_base)
{
	const struct side_type *elem_type = side_gather_container_elem_type(container);
	const char *base;
	uint32_t stride;
	uint64_t length;

	if (!elem_type || !value_base)
		return -EINVAL;
	stride = side_gather_elem_stride(elem_type);
	if (!stride)
		return -EINVAL;
	switch (side_enum_get(container->type)) {
	case SIDE_TYPE_GATHER_ARRAY:
	{
		const struct side_type_gather_array *ga =
			&container->u.side_gather.u.side_array;

		length = ga->type.length;
		base = side_gather_access(side_enum_get(ga->access_mode),
			(const char *) value_base + ga->offset);
		break;
	}
	case SIDE_TYPE_GATHER_VLA:
	{
		const struct side_type_gather_vla *gv =
			&container->u.side_gather.u.side_vla;
		const struct side_type *length_type =
			side_ptr_rel_get(gv->type.length_type);
		int64_t v;

		/* The length of a gathered sequence is gathered as well. */
		if (!length_base
				|| side_enum_get(length_type->type) != SIDE_TYPE_GATHER_INTEGER)
			return -EINVAL;
		if (side_gather_load_integer(
				&length_type->u.side_gather.u.side_integer,
				length_base, false, false, &v))
			return -EINVAL;
		if (v < 0)
			return -EINVAL;
		length = (uint64_t) v;
		base = side_gather_access(side_enum_get(gv->access_mode),
			(const char *) value_base + gv->offset);
		break;
	}
	default:
		return -EINVAL;
	}
	if (!base || index >= length)
		return -EINVAL;
	*elem_base = base + index * stride;
	return 0;
}

/*
 * The two addresses a gathered container is reached from within an
 * argument: the one of its elements, and the one of its length, which
 * a sequence carries separately.
 */
void side_gather_container_arg_base(const struct side_type *container,
		const struct side_arg *item,
		const void **value_base, const void **length_base)
{
	switch (side_enum_get(container->type)) {
	case SIDE_TYPE_GATHER_VLA:
		*value_base = side_ptr_get(item->u.side_static.side_vla_gather.ptr);
		*length_base = side_ptr_get(item->u.side_static.side_vla_gather.length_ptr);
		break;
	default:
		*value_base = side_ptr_get(item->u.side_static.side_array_gather_ptr);
		*length_base = *value_base;
		break;
	}
}

static
int side_struct_member_lookup(const struct side_type_struct *side_struct,
		const char *name, size_t len, const struct side_type **type)
{
	uint32_t i, nr_fields = side_array_length(&side_struct->fields);

	for (i = 0; i < nr_fields; i++) {
		const struct side_event_field *field =
			side_array_rel_at(&side_struct->fields, i);
		const char *field_name = side_ptr_rel_get(field->field_name);

		if (strlen(field_name) == len && !strncmp(field_name, name, len)) {
			*type = &field->side_type;
			return (int) i;
		}
	}
	return -1;
}

int side_struct_member_lookup_by_name(const struct side_type *side_type,
		const char *name, uint64_t *idx, const struct side_type **member_type)
{
	const struct side_type_struct *side_struct = side_type_struct_fields(side_type);
	int ret;

	if (!side_struct)
		return -1;
	ret = side_struct_member_lookup(side_struct, name, strlen(name), member_type);
	if (ret < 0)
		return -1;
	*idx = (uint64_t) ret;
	return 0;
}

/* The type of the member at @idx of a structure, gathered or not. */
const struct side_type *side_struct_member_type(const struct side_type *side_type,
		uint64_t idx)
{
	const struct side_type_struct *side_struct = side_type_struct_fields(side_type);

	if (!side_struct || idx >= side_array_length(&side_struct->fields))
		return NULL;
	return &((const struct side_event_field *)
		side_array_rel_at(&side_struct->fields, idx))->side_type;
}

/*
 * The address the members of a gathered structure are read from, which
 * each of them applies its own offset to.
 */
int side_gather_struct_base(const struct side_type *side_type,
		const void *base, const void **member_base)
{
	const struct side_type_gather_struct *gs;
	const char *ptr;

	if (side_enum_get(side_type->type) != SIDE_TYPE_GATHER_STRUCT || !base)
		return -EINVAL;
	gs = &side_type->u.side_gather.u.side_struct;
	ptr = side_gather_access(side_enum_get(gs->access_mode),
			(const char *) base + gs->offset);
	if (!ptr)
		return -EINVAL;
	*member_base = ptr;
	return 0;
}

int lttng_bytecode_side_field_path(const struct side_event_description *side_desc,
		const char *name, struct side_field_path *path,
		const struct side_type **type)
{
	const struct side_type *current = NULL;
	uint32_t i, nr_fields;

	if (!side_desc)
		return -1;
	path->nr = 0;
	for (;;) {
		const char *dot = strchr(name, '.');
		size_t len = dot ? (size_t) (dot - name) : strlen(name);
		int idx;

		if (!len || path->nr >= SIDE_FIELD_PATH_MAX)
			return -1;
		if (!current) {
			/* A field of the event. */
			nr_fields = side_array_length(&side_desc->fields);
			idx = -1;
			for (i = 0; i < nr_fields; i++) {
				const struct side_event_field *field =
					side_array_rel_at(&side_desc->fields, i);
				const char *field_name = side_ptr_rel_get(field->field_name);

				if (strlen(field_name) == len
						&& !strncmp(field_name, name, len)) {
					current = &field->side_type;
					idx = (int) i;
					break;
				}
			}
		} else {
			/* A member of the structure reached so far. */
			const struct side_type_struct *side_struct =
				side_type_struct_fields(current);

			if (!side_struct)
				return -1;
			idx = side_struct_member_lookup(side_struct, name, len, &current);
		}
		if (idx < 0)
			return -1;
		path->idx[path->nr++] = (uint16_t) idx;
		if (!dot)
			break;
		name = dot + 1;
	}
	*type = current;
	return 0;
}

/* The address a gather type applies its own offset and access to. */
static
const void *side_arg_gather_base(const struct side_arg *item)
{
	/*
	 * The pointer of every gather type is the same member of the
	 * argument union, under a name per type.
	 */
	return side_ptr_get(item->u.side_static.side_integer_gather_ptr);
}

int lttng_bytecode_side_field_ref(const struct side_event_description *side_desc,
		const struct side_field_path *path,
		const struct side_arg_vec *sav,
		struct side_field_ref *ref)
{
	const struct side_type *type;
	const struct side_arg *arg;
	const void *base = NULL;
	uint16_t i;

	if (!side_desc || !path->nr)
		return -EINVAL;
	if (path->idx[0] >= sav->len
			|| path->idx[0] >= side_array_length(&side_desc->fields))
		return -EINVAL;
	type = &((const struct side_event_field *)
		side_array_rel_at(&side_desc->fields, path->idx[0]))->side_type;
	arg = &side_ptr_get(sav->sav)[path->idx[0]];
	for (i = 1; i < path->nr; i++) {
		const struct side_type_struct *side_struct;

		switch (side_enum_get(type->type)) {
		case SIDE_TYPE_STRUCT:
		{
			const struct side_arg_vec *sub =
				side_ptr_get(arg->u.side_static.side_struct);

			side_struct = side_ptr_sel_get(type->u.side_struct);
			if (path->idx[i] >= sub->len
					|| path->idx[i] >= side_array_length(&side_struct->fields))
				return -EINVAL;
			arg = &side_ptr_get(sub->sav)[path->idx[i]];
			type = &((const struct side_event_field *)
				side_array_rel_at(&side_struct->fields, path->idx[i]))->side_type;
			break;
		}
		case SIDE_TYPE_GATHER_STRUCT:
		{
			const struct side_type_gather_struct *gs =
				&type->u.side_gather.u.side_struct;
			const char *ptr;

			/*
			 * The members of a gathered structure are not
			 * arguments of their own: they are read from the
			 * memory the structure resolves to, each member
			 * applying its own offset to it.
			 */
			if (!base)
				base = side_arg_gather_base(arg);
			ptr = side_gather_access(side_enum_get(gs->access_mode),
					(const char *) base + gs->offset);
			if (!ptr)
				return -EINVAL;
			base = ptr;
			side_struct = side_ptr_sel_get(gs->type);
			if (path->idx[i] >= side_array_length(&side_struct->fields))
				return -EINVAL;
			type = &((const struct side_event_field *)
				side_array_rel_at(&side_struct->fields, path->idx[i]))->side_type;
			break;
		}
		default:
			return -EINVAL;
		}
	}
	ref->type = type;
	if (base) {
		ref->arg = NULL;
		ref->gather_base = base;
	} else {
		ref->arg = arg;
		ref->gather_base = side_arg_is_gather(type) ?
			side_arg_gather_base(arg) : NULL;
	}
	return 0;
}

int lttng_bytecode_side_field_lookup(const struct side_event_description *side_desc,
		const char *name, const struct side_type **type)
{
	uint32_t i, nr_fields;

	nr_fields = side_array_length(&side_desc->fields);
	for (i = 0; i < nr_fields; i++) {
		const struct side_event_field *field =
			side_array_rel_at(&side_desc->fields, i);

		if (!strcmp(side_ptr_rel_get(field->field_name), name)) {
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
	struct side_field_path path;
	ssize_t data_offset;
	struct load_op *op;

	dbg_printf("Apply side field reloc: %u %s\n", reloc_offset, field_name);

	if (!event_desc)
		return -EINVAL;
	side_desc = lttng_ust_side_get_side_desc(event_desc);
	if (!side_desc)
		return -EINVAL;
	/*
	 * The session daemon emits a chain of symbols as one dotted
	 * name, so a field of a structure arrives here as "a.b".
	 */
	if (lttng_bytecode_side_field_path(side_desc, field_name, &path, &side_type))
		return -EINVAL;
	data_offset = lttng_bytecode_side_push_data(runtime, &path,
			__alignof__(path), sizeof(path));
	if (data_offset < 0)
		return -EINVAL;
	/* The operand of a field reference is 16-bit. */
	if (data_offset > UINT16_MAX)
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
		/*
		 * A gathered value compares as the value it wraps. The
		 * interpreter reads it from the address the argument
		 * carries, which it reaches through the description this
		 * bytecode is linked against.
		 */
		case SIDE_TYPE_GATHER_INTEGER:	/* Fall-through. */
		case SIDE_TYPE_GATHER_POINTER:	/* Fall-through. */
		case SIDE_TYPE_GATHER_BOOL:	/* Fall-through. */
		case SIDE_TYPE_GATHER_BYTE:	/* Fall-through. */
		case SIDE_TYPE_GATHER_ENUM:
			op->op = BYTECODE_OP_LOAD_FIELD_REF_S64;
			break;
		case SIDE_TYPE_GATHER_FLOAT:
			op->op = BYTECODE_OP_LOAD_FIELD_REF_DOUBLE;
			break;
		case SIDE_TYPE_GATHER_STRING:
			op->op = BYTECODE_OP_LOAD_FIELD_REF_STRING;
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
		/* The operand is where the path was resolved. */
		field_ref->offset = (uint16_t) data_offset;
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
