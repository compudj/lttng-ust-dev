/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2010-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * LTTng UST bytecode interpreter for side events.
 *
 * Adapted from lttng-bytecode-interpreter.c: the interpreter input
 * is the SIDE payload ABI (const struct side_arg_vec *) instead of
 * the marshalled interpreter stack data. Payload field references
 * carry a side argument index, and payload object pointers designate
 * self-describing typed side arguments (marked with
 * side_arg_field_marker). Indexing an array or a VLA indexes its
 * nested argument vector, so its elements are self-describing side
 * arguments as well. Context references and objects are interpreted
 * exactly like for tracepoint events.
 */

#define _LGPL_SOURCE
#include <stddef.h>
#include <stdint.h>

#include <lttng/urcu/pointer.h>
#include <urcu/rculist.h>
#include <lttng/ust-endian.h>
#include <lttng/ust-events.h>
#include "lib/lttng-ust/events.h"

#include <side/trace.h>

#include "lttng-bytecode.h"
#include "lttng-bytecode-side.h"
#include "common/strutils.h"


/*
 * -1: wildcard found.
 * -2: unknown escape char.
 * 0: normal char.
 */

static
int parse_char(const char **p)
{
	switch (**p) {
	case '\\':
		(*p)++;
		switch (**p) {
		case '\\':
		case '*':
			return 0;
		default:
			return -2;
		}
	case '*':
		return -1;
	default:
		return 0;
	}
}

/*
 * Returns SIZE_MAX if the string is null-terminated, or the number of
 * characters if not.
 */
static
size_t get_str_or_seq_len(const struct estack_entry *entry)
{
	return entry->u.s.seq_len;
}

static
int stack_star_glob_match(struct estack *stack, int top,
		const char *cmp_type __attribute__((unused)))
{
	const char *pattern;
	const char *candidate;
	size_t pattern_len;
	size_t candidate_len;

	/* Find out which side is the pattern vs. the candidate. */
	if (estack_ax(stack, top)->u.s.literal_type == ESTACK_STRING_LITERAL_TYPE_STAR_GLOB) {
		pattern = estack_ax(stack, top)->u.s.str;
		pattern_len = get_str_or_seq_len(estack_ax(stack, top));
		candidate = estack_bx(stack, top)->u.s.str;
		candidate_len = get_str_or_seq_len(estack_bx(stack, top));
	} else {
		pattern = estack_bx(stack, top)->u.s.str;
		pattern_len = get_str_or_seq_len(estack_bx(stack, top));
		candidate = estack_ax(stack, top)->u.s.str;
		candidate_len = get_str_or_seq_len(estack_ax(stack, top));
	}

	/* Perform the match. Returns 0 when the result is true. */
	return !strutils_star_glob_match(pattern, pattern_len, candidate,
		candidate_len);
}

static
int stack_strcmp(struct estack *stack, int top, const char *cmp_type __attribute__((unused)))
{
	const char *p = estack_bx(stack, top)->u.s.str, *q = estack_ax(stack, top)->u.s.str;
	int ret;
	int diff;

	for (;;) {
		int escaped_r0 = 0;

		if (unlikely(p - estack_bx(stack, top)->u.s.str >= estack_bx(stack, top)->u.s.seq_len || *p == '\0')) {
			if (q - estack_ax(stack, top)->u.s.str >= estack_ax(stack, top)->u.s.seq_len || *q == '\0') {
				return 0;
			} else {
				if (estack_ax(stack, top)->u.s.literal_type ==
						ESTACK_STRING_LITERAL_TYPE_PLAIN) {
					ret = parse_char(&q);
					if (ret == -1)
						return 0;
				}
				return -1;
			}
		}
		if (unlikely(q - estack_ax(stack, top)->u.s.str >= estack_ax(stack, top)->u.s.seq_len || *q == '\0')) {
			if (estack_bx(stack, top)->u.s.literal_type ==
					ESTACK_STRING_LITERAL_TYPE_PLAIN) {
				ret = parse_char(&p);
				if (ret == -1)
					return 0;
			}
			return 1;
		}
		if (estack_bx(stack, top)->u.s.literal_type ==
				ESTACK_STRING_LITERAL_TYPE_PLAIN) {
			ret = parse_char(&p);
			if (ret == -1) {
				return 0;
			} else if (ret == -2) {
				escaped_r0 = 1;
			}
			/* else compare both char */
		}
		if (estack_ax(stack, top)->u.s.literal_type ==
				ESTACK_STRING_LITERAL_TYPE_PLAIN) {
			ret = parse_char(&q);
			if (ret == -1) {
				return 0;
			} else if (ret == -2) {
				if (!escaped_r0)
					return -1;
			} else {
				if (escaped_r0)
					return 1;
			}
		} else {
			if (escaped_r0)
				return 1;
		}
		diff = *p - *q;
		if (diff != 0)
			break;
		p++;
		q++;
	}
	return diff;
}

/* lttng_bytecode_interpret_error is shared with the tracepoint interpreter. */

#ifdef INTERPRETER_USE_SWITCH

/*
 * Fallback for compilers that do not support taking address of labels.
 */

#define START_OP							\
	start_pc = &bytecode->code[0];					\
	for (pc = next_pc = start_pc; pc - start_pc < bytecode->len;	\
			pc = next_pc) {					\
		dbg_printf("Executing op %s (%u)\n",			\
			lttng_bytecode_print_op((unsigned int) *(bytecode_opcode_t *) pc), \
			(unsigned int) *(bytecode_opcode_t *) pc); 	\
		switch (*(bytecode_opcode_t *) pc)	{

#define OP(name)	jump_target_##name: __attribute__((unused));	\
			case name

#define PO		break

#define END_OP		}						\
	}

#define JUMP_TO(name)							\
			goto jump_target_##name

#else

/*
 * Dispatch-table based interpreter.
 */

#define START_OP							\
	start_pc = &bytecode->code[0];					\
	pc = next_pc = start_pc;					\
	if (unlikely(pc - start_pc >= bytecode->len))			\
		goto end;						\
	goto *dispatch[*(bytecode_opcode_t *) pc];

#define OP(name)							\
LABEL_##name

#define PO								\
		pc = next_pc;						\
		goto *dispatch[*(bytecode_opcode_t *) pc];

#define END_OP

#define JUMP_TO(name)							\
		goto LABEL_##name

#endif

#define IS_INTEGER_REGISTER(reg_type) \
		(reg_type == REG_U64 || reg_type == REG_S64)

static int context_get_index(struct lttng_ust_ctx *ctx,
		struct lttng_ust_probe_ctx *probe_ctx,
		struct load_ptr *ptr,
		uint32_t idx)
{

	const struct lttng_ust_ctx_field *ctx_field;
	const struct lttng_ust_event_field *field;
	struct lttng_ust_ctx_value v;

	ctx_field = &ctx->fields[idx];
	field = ctx_field->event_field;
	ptr->type = LOAD_OBJECT;
	ptr->field = field;

	switch (field->type->type) {
	case lttng_ust_type_integer:
		ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
		if (lttng_ust_get_type_integer(field->type)->signedness) {
			ptr->object_type = OBJECT_TYPE_S64;
			ptr->u.s64 = v.u.s64;
			ptr->ptr = &ptr->u.s64;
		} else {
			ptr->object_type = OBJECT_TYPE_U64;
			ptr->u.u64 = v.u.s64;	/* Cast. */
			ptr->ptr = &ptr->u.u64;
		}
		ptr->rev_bo = lttng_ust_get_type_integer(field->type)->reverse_byte_order;
		break;
	case lttng_ust_type_enum:
	{
		const struct lttng_ust_type_integer *itype;

		itype = lttng_ust_get_type_integer(lttng_ust_get_type_enum(field->type)->container_type);
		ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
		if (itype->signedness) {
			ptr->object_type = OBJECT_TYPE_SIGNED_ENUM;
			ptr->u.s64 = v.u.s64;
			ptr->ptr = &ptr->u.s64;
		} else {
			ptr->object_type = OBJECT_TYPE_UNSIGNED_ENUM;
			ptr->u.u64 = v.u.s64;	/* Cast. */
			ptr->ptr = &ptr->u.u64;
		}
		ptr->rev_bo = itype->reverse_byte_order;
		break;
	}
	case lttng_ust_type_array:
		if (lttng_ust_get_type_array(field->type)->elem_type->type != lttng_ust_type_integer) {
			ERR("Array nesting only supports integer types.");
			return -EINVAL;
		}
		if (lttng_ust_get_type_array(field->type)->encoding == lttng_ust_string_encoding_none) {
			ERR("Only string arrays are supported for contexts.");
			return -EINVAL;
		}
		ptr->object_type = OBJECT_TYPE_STRING;
		ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
		ptr->ptr = v.u.str;
		break;
	case lttng_ust_type_sequence:
		if (lttng_ust_get_type_sequence(field->type)->elem_type->type != lttng_ust_type_integer) {
			ERR("Sequence nesting only supports integer types.");
			return -EINVAL;
		}
		if (lttng_ust_get_type_sequence(field->type)->encoding == lttng_ust_string_encoding_none) {
			ERR("Only string sequences are supported for contexts.");
			return -EINVAL;
		}
		ptr->object_type = OBJECT_TYPE_STRING;
		ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
		ptr->ptr = v.u.str;
		break;
	case lttng_ust_type_string:
		ptr->object_type = OBJECT_TYPE_STRING;
		ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
		ptr->ptr = v.u.str;
		break;
	case lttng_ust_type_float:
		ptr->object_type = OBJECT_TYPE_DOUBLE;
		ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
		ptr->u.d = v.u.d;
		ptr->ptr = &ptr->u.d;
		ptr->rev_bo = lttng_ust_get_type_float(field->type)->reverse_byte_order;
		break;
	case lttng_ust_type_fixed_length_blob:
		ERR("Fixed length blobs in contexts are not supported by bytecode interpreter.");
		return -EINVAL;
	case lttng_ust_type_variable_length_blob:
		ERR("Variable length blobs in contexts are not supported by bytecode interpreter.");
		return -EINVAL;
	case lttng_ust_type_dynamic:
		ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
		switch (v.sel) {
		case LTTNG_UST_DYNAMIC_TYPE_NONE:
			return -EINVAL;
		case LTTNG_UST_DYNAMIC_TYPE_U8:
		case LTTNG_UST_DYNAMIC_TYPE_U16:
		case LTTNG_UST_DYNAMIC_TYPE_U32:
		case LTTNG_UST_DYNAMIC_TYPE_U64:
			ptr->object_type = OBJECT_TYPE_U64;
			ptr->u.u64 = v.u.u64;
			ptr->ptr = &ptr->u.u64;
			/*
			 * struct lttng_ust_ctx_value does not currently
			 * feature a byte order field.
			 */
			ptr->rev_bo = false;
			dbg_printf("context get index dynamic u64 %" PRIi64 "\n", ptr->u.u64);
			break;
		case LTTNG_UST_DYNAMIC_TYPE_S8:
		case LTTNG_UST_DYNAMIC_TYPE_S16:
		case LTTNG_UST_DYNAMIC_TYPE_S32:
		case LTTNG_UST_DYNAMIC_TYPE_S64:
			ptr->object_type = OBJECT_TYPE_S64;
			ptr->u.s64 = v.u.s64;
			ptr->ptr = &ptr->u.s64;
			/*
			 * struct lttng_ust_ctx_value does not currently
			 * feature a byte order field.
			 */
			ptr->rev_bo = false;
			dbg_printf("context get index dynamic s64 %" PRIi64 "\n", ptr->u.s64);
			break;
		case LTTNG_UST_DYNAMIC_TYPE_FLOAT:
		case LTTNG_UST_DYNAMIC_TYPE_DOUBLE:
			ptr->object_type = OBJECT_TYPE_DOUBLE;
			ptr->u.d = v.u.d;
			ptr->ptr = &ptr->u.d;
			/*
			 * struct lttng_ust_ctx_value does not currently
			 * feature a byte order field.
			 */
			ptr->rev_bo = false;
			dbg_printf("context get index dynamic double %g\n", ptr->u.d);
			break;
		case LTTNG_UST_DYNAMIC_TYPE_STRING:
			ptr->object_type = OBJECT_TYPE_STRING;
			ptr->ptr = v.u.str;
			dbg_printf("context get index dynamic string %s\n", (const char *) ptr->ptr);
			break;
		default:
			dbg_printf("Interpreter warning: unknown dynamic type (%d).\n", (int) v.sel);
			return -EINVAL;
		}
		break;
	default:
		ERR("Unknown type: %d", (int) field->type->type);
		return -EINVAL;
	}
	return 0;
}

/*
 * Sentinel marking estack pointers which designate a self-describing
 * side argument (SIDE payload ABI) rather than memory: context
 * objects and array elements keep the memory-based loads.
 */
static const struct lttng_ust_event_field side_arg_field_marker;

/*
 * The elements of a gathered array or sequence are not arguments of
 * their own: indexing one leaves the address it is read from rather
 * than an argument, which this marker says.
 */
static const struct lttng_ust_event_field side_gather_base_marker;

/*
 * A gather type reads its value from an address rather than carrying
 * it in the argument, which holds the address instead. Resolve it: the
 * access mode says whether the address is the value's own or a pointer
 * to it, and the offset is applied before the access.
 */
/*
 * The address the argument of a gather field carries, NULL when the
 * field is not a gathered one.
 */
static const void *side_arg_gather_base_of(const struct side_type *side_type,
		const struct side_arg *item)
{
	if (!side_type || !side_arg_is_gather(side_type))
		return NULL;
	/*
	 * The pointer of every gather type is the same member of the
	 * argument union, under a name per type.
	 */
	return side_ptr_get(item->u.side_static.side_integer_gather_ptr);
}

/*
 * Load the value a gather field reads from memory, for the types which
 * compare as an integer. Returns -ENOENT when @side_type is not one of
 * them, which leaves the caller with the argument to load from.
 */
static int side_gather_load_field_integer(const struct side_type *side_type,
		const void *gather_base, bool rev_bo, int64_t *v)
{
	const struct side_type_gather *gather;

	if (!side_type || !gather_base)
		return -ENOENT;
	gather = &side_type->u.side_gather;
	switch (side_enum_get(side_type->type)) {
	case SIDE_TYPE_GATHER_INTEGER:
		return side_gather_load_integer(&gather->u.side_integer,
			gather_base,
			gather->u.side_integer.type.signedness, rev_bo, v);
	case SIDE_TYPE_GATHER_POINTER:
		return side_gather_load_integer(&gather->u.side_integer,
			gather_base, false, rev_bo, v);
	case SIDE_TYPE_GATHER_ENUM:
	{
		const struct side_type *container =
			side_ptr_get(gather->u.side_enum.elem_type);
		const struct side_type_gather_integer *t;

		if (side_enum_get(container->type) != SIDE_TYPE_GATHER_INTEGER)
			return -EINVAL;
		t = &container->u.side_gather.u.side_integer;
		return side_gather_load_integer(t, gather_base,
			t->type.signedness, rev_bo, v);
	}
	case SIDE_TYPE_GATHER_BOOL:
	{
		const struct side_type_gather_bool *t = &gather->u.side_bool;
		union side_bool_value value;
		const char *ptr;

		if (t->offset_bits)
			return -EINVAL;
		ptr = side_gather_access(side_enum_get(t->access_mode),
			(const char *) gather_base + t->offset);
		if (!ptr)
			return -EINVAL;
		switch (t->type.bool_size) {
		case 1:
			memcpy(&value, ptr, 1);
			*v = !!value.side_bool8;
			break;
		case 2:
			memcpy(&value, ptr, 2);
			*v = !!value.side_bool16;
			break;
		case 4:
			memcpy(&value, ptr, 4);
			*v = !!value.side_bool32;
			break;
		case 8:
			memcpy(&value, ptr, 8);
			*v = !!value.side_bool64;
			break;
		default:
			return -EINVAL;
		}
		break;
	}
	case SIDE_TYPE_GATHER_BYTE:
	{
		const struct side_type_gather_byte *t = &gather->u.side_byte;
		const char *ptr;
		uint8_t byte;

		ptr = side_gather_access(side_enum_get(t->access_mode),
			(const char *) gather_base + t->offset);
		if (!ptr)
			return -EINVAL;
		memcpy(&byte, ptr, 1);
		*v = byte;
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

/*
 * Same, for a string: what a gathered string reads from memory is the
 * string itself, at the address the access resolves to.
 */
static int side_gather_load_field_string(const struct side_type *side_type,
		const void *gather_base, const char **str)
{
	const struct side_type_gather_string *t;
	const char *ptr;

	if (!side_type || !gather_base
			|| side_enum_get(side_type->type) != SIDE_TYPE_GATHER_STRING)
		return -ENOENT;
	t = &side_type->u.side_gather.u.side_string;
	if (t->type.unit_size != 1)
		return -EINVAL;
	ptr = side_gather_access(side_enum_get(t->access_mode),
		(const char *) gather_base + t->offset);
	if (!ptr)
		return -EINVAL;
	*str = ptr;
	return 0;
}

/* Same, for the types which compare as a double. */
static int side_gather_load_field_double(const struct side_type *side_type,
		const void *gather_base, bool rev_bo, double *d)
{
	const struct side_type_gather_float *t;
	union side_float_value value;
	const char *ptr;

	if (!side_type || !gather_base
			|| side_enum_get(side_type->type) != SIDE_TYPE_GATHER_FLOAT)
		return -ENOENT;
	t = &side_type->u.side_gather.u.side_float;
	ptr = side_gather_access(side_enum_get(t->access_mode),
		(const char *) gather_base + t->offset);
	if (!ptr)
		return -EINVAL;
	switch (t->type.float_size) {
#if __HAVE_FLOAT32
	case 4:
	{
		union {
			float f;
			uint32_t u;
		} float32;

		memcpy(&value, ptr, 4);
		float32.f = value.side_float_binary32;
		if (rev_bo)
			float32.u = lttng_ust_bswap_32(float32.u);
		*d = float32.f;
		break;
	}
#endif
#if __HAVE_FLOAT64
	case 8:
	{
		union {
			double f;
			uint64_t u;
		} float64;

		memcpy(&value, ptr, 8);
		float64.f = value.side_float_binary64;
		if (rev_bo)
			float64.u = lttng_ust_bswap_64(float64.u);
		*d = float64.f;
		break;
	}
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * A value travels in the byte order it is emitted with, which the
 * event description carries and the specialize phase resolved into
 * rev_bo. The interpreter compares values, so it converts them to the
 * byte order of the host. The load is done through the unsigned member
 * of the union, which is the same storage, so that the byte swap
 * operates on the bit pattern before it is given its signedness.
 */
static int side_arg_load_integer(const struct side_arg *item, bool rev_bo,
		int64_t *v)
{
	const union side_integer_value *value =
		&item->u.side_static.integer_value;

	switch (side_enum_get(item->type)) {
	case SIDE_TYPE_S8:
		*v = (int8_t) value->side_u8;
		break;
	case SIDE_TYPE_S16:
	{
		uint16_t tmp = value->side_u16;

		if (rev_bo)
			tmp = lttng_ust_bswap_16(tmp);
		*v = (int16_t) tmp;
		break;
	}
	case SIDE_TYPE_S32:
	{
		uint32_t tmp = value->side_u32;

		if (rev_bo)
			tmp = lttng_ust_bswap_32(tmp);
		*v = (int32_t) tmp;
		break;
	}
	case SIDE_TYPE_S64:
	{
		uint64_t tmp = value->side_u64;

		if (rev_bo)
			tmp = lttng_ust_bswap_64(tmp);
		*v = (int64_t) tmp;
		break;
	}
	case SIDE_TYPE_U8:
		*v = (int64_t) value->side_u8;
		break;
	case SIDE_TYPE_U16:
	{
		uint16_t tmp = value->side_u16;

		if (rev_bo)
			tmp = lttng_ust_bswap_16(tmp);
		*v = (int64_t) tmp;
		break;
	}
	case SIDE_TYPE_U32:
	{
		uint32_t tmp = value->side_u32;

		if (rev_bo)
			tmp = lttng_ust_bswap_32(tmp);
		*v = (int64_t) tmp;
		break;
	}
	case SIDE_TYPE_U64:
	{
		uint64_t tmp = value->side_u64;

		if (rev_bo)
			tmp = lttng_ust_bswap_64(tmp);
		*v = (int64_t) tmp;
		break;
	}
	case SIDE_TYPE_BOOL:
		/* Stack-copy bool arguments are stored as bool8. */
		*v = !!item->u.side_static.bool_value.side_bool8;
		break;
	case SIDE_TYPE_BYTE:
		/* A single byte has no byte order. */
		*v = item->u.side_static.byte_value;
		break;
	case SIDE_TYPE_POINTER:
	{
		uintptr_t tmp = value->side_uptr;

		if (rev_bo) {
			if (sizeof(tmp) == sizeof(uint64_t))
				tmp = (uintptr_t) lttng_ust_bswap_64((uint64_t) tmp);
			else
				tmp = (uintptr_t) lttng_ust_bswap_32((uint32_t) tmp);
		}
		*v = (int64_t) tmp;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

static int side_arg_load_double(const struct side_arg *item, bool rev_bo,
		double *d)
{
	switch (side_enum_get(item->type)) {
#if __HAVE_FLOAT32
	case SIDE_TYPE_FLOAT_BINARY32:
	{
		union {
			float f;
			uint32_t u;
		} float32 = {
			.f = item->u.side_static.float_value.side_float_binary32,
		};

		if (rev_bo)
			float32.u = lttng_ust_bswap_32(float32.u);
		*d = float32.f;
		break;
	}
#endif
#if __HAVE_FLOAT64
	case SIDE_TYPE_FLOAT_BINARY64:
	{
		union {
			double f;
			uint64_t u;
		} float64 = {
			.f = item->u.side_static.float_value.side_float_binary64,
		};

		if (rev_bo)
			float64.u = lttng_ust_bswap_64(float64.u);
		*d = float64.f;
		break;
	}
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

static int side_arg_load_string(const struct side_arg *item, const char **str)
{
	switch (side_enum_get(item->type)) {
	case SIDE_TYPE_STRING_UTF8:
		*str = (const char *) side_ptr_get(item->u.side_static.string_value);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * Object type of a side argument, for arguments typed by the emitted
 * event rather than by the event description: array and VLA elements.
 */
static int side_arg_object_type(const struct side_arg *item,
		enum object_type *object_type)
{
	switch (side_enum_get(item->type)) {
	case SIDE_TYPE_S8:		/* Fall-through. */
	case SIDE_TYPE_S16:		/* Fall-through. */
	case SIDE_TYPE_S32:		/* Fall-through. */
	case SIDE_TYPE_S64:
		*object_type = OBJECT_TYPE_S64;
		break;
	case SIDE_TYPE_U8:		/* Fall-through. */
	case SIDE_TYPE_U16:		/* Fall-through. */
	case SIDE_TYPE_U32:		/* Fall-through. */
	case SIDE_TYPE_U64:		/* Fall-through. */
	case SIDE_TYPE_BOOL:		/* Fall-through. */
	case SIDE_TYPE_BYTE:		/* Fall-through. */
	case SIDE_TYPE_POINTER:
		*object_type = OBJECT_TYPE_U64;
		break;
	case SIDE_TYPE_FLOAT_BINARY16:	/* Fall-through. */
	case SIDE_TYPE_FLOAT_BINARY32:	/* Fall-through. */
	case SIDE_TYPE_FLOAT_BINARY64:	/* Fall-through. */
	case SIDE_TYPE_FLOAT_BINARY128:
		*object_type = OBJECT_TYPE_DOUBLE;
		break;
	case SIDE_TYPE_STRING_UTF8:	/* Fall-through. */
	case SIDE_TYPE_STRING_UTF16:	/* Fall-through. */
	case SIDE_TYPE_STRING_UTF32:
		*object_type = OBJECT_TYPE_STRING;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* Load the value at the address a gathered element resolved to. */
static int side_gather_dynamic_load_field(struct estack_entry *stack_top)
{
	/*
	 * The value loaded is written to the same storage the address
	 * and the type it is loaded from occupy: they are members of
	 * one union. Everything the load needs is read out of it first.
	 */
	enum object_type object_type = stack_top->u.ptr.object_type;
	const struct side_type *side_type = stack_top->u.ptr.side_type;
	const void *base = stack_top->u.ptr.ptr;
	bool rev_bo = stack_top->u.ptr.rev_bo;
	int ret;

	switch (object_type) {
	case OBJECT_TYPE_S64:		/* Fall-through. */
	case OBJECT_TYPE_U64:
	{
		int64_t v;

		ret = side_gather_load_field_integer(side_type, base, rev_bo, &v);
		if (ret)
			return ret;
		stack_top->u.v = v;
		stack_top->type = object_type == OBJECT_TYPE_S64 ?
			REG_S64 : REG_U64;
		break;
	}
	case OBJECT_TYPE_DOUBLE:
	{
		double d;

		ret = side_gather_load_field_double(side_type, base, rev_bo, &d);
		if (ret)
			return ret;
		stack_top->u.d = d;
		stack_top->type = REG_DOUBLE;
		break;
	}
	case OBJECT_TYPE_STRING:
	{
		const char *str;

		ret = side_gather_load_field_string(side_type, base, &str);
		if (ret)
			return ret;
		if (unlikely(!str)) {
			dbg_printf("Interpreter warning: loading a NULL string.\n");
			return -EINVAL;
		}
		stack_top->u.s.str = str;
		stack_top->u.s.seq_len = SIZE_MAX;
		stack_top->u.s.literal_type = ESTACK_STRING_LITERAL_TYPE_NONE;
		stack_top->type = REG_STRING;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

/* Load a side argument onto the estack entry. */
static int side_arg_dynamic_load_field(struct estack_entry *stack_top)
{
	const struct side_arg *item =
		(const struct side_arg *) stack_top->u.ptr.ptr;
	enum object_type object_type = stack_top->u.ptr.object_type;
	int ret;

	if (object_type == OBJECT_TYPE_DYNAMIC) {
		ret = side_arg_object_type(item, &object_type);
		if (ret)
			return ret;
	}
	switch (object_type) {
	case OBJECT_TYPE_S64:
	{
		int64_t v;

		ret = side_gather_load_field_integer(stack_top->u.ptr.side_type,
				side_arg_gather_base_of(stack_top->u.ptr.side_type, item),
				stack_top->u.ptr.rev_bo, &v);
		if (ret == -ENOENT)
			ret = side_arg_load_integer(item, stack_top->u.ptr.rev_bo, &v);
		if (ret)
			return ret;
		stack_top->u.v = v;
		stack_top->type = REG_S64;
		break;
	}
	case OBJECT_TYPE_U64:
	{
		int64_t v;

		ret = side_gather_load_field_integer(stack_top->u.ptr.side_type,
				side_arg_gather_base_of(stack_top->u.ptr.side_type, item),
				stack_top->u.ptr.rev_bo, &v);
		if (ret == -ENOENT)
			ret = side_arg_load_integer(item, stack_top->u.ptr.rev_bo, &v);
		if (ret)
			return ret;
		stack_top->u.v = v;
		stack_top->type = REG_U64;
		break;
	}
	case OBJECT_TYPE_DOUBLE:
	{
		double d;

		ret = side_gather_load_field_double(stack_top->u.ptr.side_type,
				side_arg_gather_base_of(stack_top->u.ptr.side_type, item),
				stack_top->u.ptr.rev_bo, &d);
		if (ret == -ENOENT)
			ret = side_arg_load_double(item, stack_top->u.ptr.rev_bo, &d);
		if (ret)
			return ret;
		stack_top->u.d = d;
		stack_top->type = REG_DOUBLE;
		break;
	}
	case OBJECT_TYPE_STRING:
	{
		const char *str;

		ret = side_gather_load_field_string(stack_top->u.ptr.side_type,
				side_arg_gather_base_of(stack_top->u.ptr.side_type, item),
				&str);
		if (ret == -ENOENT)
			ret = side_arg_load_string(item, &str);
		if (ret)
			return ret;
		if (unlikely(!str)) {
			dbg_printf("Interpreter warning: loading a NULL string.\n");
			return -EINVAL;
		}
		stack_top->u.s.str = str;
		stack_top->u.s.seq_len = SIZE_MAX;
		stack_top->u.s.literal_type = ESTACK_STRING_LITERAL_TYPE_NONE;
		stack_top->type = REG_STRING;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

static int dynamic_get_index(struct lttng_ust_ctx *ctx,
		struct lttng_ust_probe_ctx *probe_ctx,
		struct bytecode_runtime *runtime,
		uint64_t index, struct estack_entry *stack_top)
{
	int ret;
	const struct bytecode_get_index_data *gid;

	gid = (const struct bytecode_get_index_data *) &runtime->data[index];
	switch (stack_top->u.ptr.type) {
	case LOAD_OBJECT:
		/*
		 * What is descended into is what the specialize phase
		 * resolved, rather than the type the value would be
		 * loaded as: the element of a stack-copy container is
		 * typed by its argument, and is a structure all the
		 * same.
		 */
		if (side_type_is_struct(gid->side_type))
			goto get_index_struct;
		switch (stack_top->u.ptr.object_type) {
		case OBJECT_TYPE_ARRAY:		/* Fall-through. */
		case OBJECT_TYPE_SEQUENCE:
		{
			const struct side_arg *item =
				(const struct side_arg *) stack_top->u.ptr.ptr;
			const struct side_arg_vec *sav;

			if (stack_top->u.ptr.field != &side_arg_field_marker
					&& stack_top->u.ptr.field != &side_gather_base_marker) {
				ret = -EINVAL;
				goto end;
			}
			if (gid->side_type
					&& side_type_is_gather_container(gid->side_type)) {
				const void *elem_base, *value_base, *length_base;

				if (stack_top->u.ptr.field == &side_gather_base_marker) {
					/*
					 * Reached through the field of a
					 * gathered structure: both the
					 * elements and the length are
					 * read from what it resolved to.
					 */
					value_base = stack_top->u.ptr.ptr;
					length_base = value_base;
				} else {
					side_gather_container_arg_base(gid->side_type,
						item, &value_base, &length_base);
				}
				ret = side_gather_container_elem(gid->side_type,
					value_base, length_base, gid->offset,
					&elem_base);
				if (ret)
					goto end;
				stack_top->u.ptr.ptr = elem_base;
				stack_top->u.ptr.object_type = gid->elem.type;
				stack_top->u.ptr.rev_bo = gid->elem.rev_bo;
				stack_top->u.ptr.side_type =
					side_gather_container_elem_type(gid->side_type);
				/* The element is an address, not an argument. */
				stack_top->u.ptr.field = &side_gather_base_marker;
				break;
			}
			/* Arrays and VLAs are nested argument vectors. */
			switch (side_enum_get(item->type)) {
			case SIDE_TYPE_ARRAY:
				sav = side_ptr_get(item->u.side_static.side_array);
				break;
			case SIDE_TYPE_VLA:
				sav = side_ptr_get(item->u.side_static.side_vla);
				break;
			default:
				ret = -EINVAL;
				goto end;
			}
			if (gid->offset >= sav->len) {
				ret = -EINVAL;
				goto end;
			}
			stack_top->u.ptr.ptr =
				(const char *) &side_ptr_get(sav->sav)[gid->offset];
			stack_top->u.ptr.object_type = gid->elem.type;
			stack_top->u.ptr.rev_bo = gid->elem.rev_bo;
			/*
			 * The value of an element is typed by the
			 * argument, but its type is what a symbol
			 * resolved against it is looked up in.
			 */
			stack_top->u.ptr.side_type = gid->side_type;
			/* The element is itself a side argument. */
			break;
		}
		case OBJECT_TYPE_STRUCT:
		get_index_struct:
		{
			const struct side_type *container = gid->side_type;
			const struct side_type *member_type;

			if (!container) {
				ret = -EINVAL;
				goto end;
			}
			member_type = side_struct_member_type(container, gid->offset);
			if (!member_type) {
				ret = -EINVAL;
				goto end;
			}
			switch (side_enum_get(container->type)) {
			case SIDE_TYPE_STRUCT:
			{
				const struct side_arg *item;
				const struct side_arg_vec *sub;

				/* Its members are arguments of their own. */
				if (stack_top->u.ptr.field != &side_arg_field_marker) {
					ret = -EINVAL;
					goto end;
				}
				item = (const struct side_arg *) stack_top->u.ptr.ptr;
				sub = side_ptr_get(item->u.side_static.side_struct);
				if (gid->offset >= sub->len) {
					ret = -EINVAL;
					goto end;
				}
				stack_top->u.ptr.ptr =
					(const char *) &side_ptr_get(sub->sav)[gid->offset];
				break;
			}
			case SIDE_TYPE_GATHER_STRUCT:
			{
				const void *base;

				/*
				 * Its members are read from the memory
				 * it resolves to, each applying its own
				 * offset to it.
				 */
				if (stack_top->u.ptr.field == &side_arg_field_marker)
					base = side_arg_gather_base_of(container,
						(const struct side_arg *) stack_top->u.ptr.ptr);
				else if (stack_top->u.ptr.field == &side_gather_base_marker)
					base = stack_top->u.ptr.ptr;
				else {
					ret = -EINVAL;
					goto end;
				}
				ret = side_gather_struct_base(container, base, &base);
				if (ret)
					goto end;
				stack_top->u.ptr.ptr = base;
				stack_top->u.ptr.field = &side_gather_base_marker;
				break;
			}
			default:
				ret = -EINVAL;
				goto end;
			}
			stack_top->u.ptr.object_type = gid->elem.type;
			stack_top->u.ptr.rev_bo = gid->elem.rev_bo;
			stack_top->u.ptr.side_type = member_type;
			break;
		}
		case OBJECT_TYPE_VARIANT:
		default:
			ERR("Unexpected get index type %d",
				(int) stack_top->u.ptr.object_type);
			ret = -EINVAL;
			goto end;
		}
		break;
	case LOAD_ROOT_CONTEXT:
	case LOAD_ROOT_APP_CONTEXT:	/* Fall-through */
	{
		ret = context_get_index(ctx,
				probe_ctx,
				&stack_top->u.ptr,
				gid->ctx_index);
		if (ret) {
			goto end;
		}
		break;
	}
	case LOAD_ROOT_PAYLOAD:
	{
		/* The payload root is the SIDE argument vector. */
		const struct side_arg_vec *sav =
			(const struct side_arg_vec *) stack_top->u.ptr.ptr;

		if (gid->offset >= sav->len) {
			ret = -EINVAL;
			goto end;
		}
		stack_top->u.ptr.ptr =
			(const char *) &side_ptr_get(sav->sav)[gid->offset];
		stack_top->u.ptr.object_type = gid->elem.type;
		stack_top->u.ptr.type = LOAD_OBJECT;
		/* Mark the object as a side argument. */
		stack_top->u.ptr.field = &side_arg_field_marker;
		stack_top->u.ptr.side_type = gid->side_type;
		stack_top->u.ptr.rev_bo = gid->elem.rev_bo;
		break;
	}
	}

	stack_top->type = REG_PTR;

	return 0;

end:
	return ret;
}

static int dynamic_load_field(struct estack_entry *stack_top)
{
	int ret;

	switch (stack_top->u.ptr.type) {
	case LOAD_OBJECT:
		break;
	case LOAD_ROOT_CONTEXT:
	case LOAD_ROOT_APP_CONTEXT:
	case LOAD_ROOT_PAYLOAD:
	default:
		dbg_printf("Interpreter warning: cannot load root, missing field name.\n");
		ret = -EINVAL;
		goto end;
	}
	/* Side arguments: self-describing typed load. */
	if (stack_top->u.ptr.field == &side_arg_field_marker)
		return side_arg_dynamic_load_field(stack_top);
	/* The element of a gathered container: an address and a type. */
	if (stack_top->u.ptr.field == &side_gather_base_marker)
		return side_gather_dynamic_load_field(stack_top);
	switch (stack_top->u.ptr.object_type) {
	case OBJECT_TYPE_S8:
		dbg_printf("op load field s8\n");
		stack_top->u.v = *(int8_t *) stack_top->u.ptr.ptr;
		stack_top->type = REG_S64;
		break;
	case OBJECT_TYPE_S16:
	{
		int16_t tmp;

		dbg_printf("op load field s16\n");
		tmp = *(int16_t *) stack_top->u.ptr.ptr;
		if (stack_top->u.ptr.rev_bo)
			tmp = lttng_ust_bswap_16(tmp);
		stack_top->u.v = tmp;
		stack_top->type = REG_S64;
		break;
	}
	case OBJECT_TYPE_S32:
	{
		int32_t tmp;

		dbg_printf("op load field s32\n");
		tmp = *(int32_t *) stack_top->u.ptr.ptr;
		if (stack_top->u.ptr.rev_bo)
			tmp = lttng_ust_bswap_32(tmp);
		stack_top->u.v = tmp;
		stack_top->type = REG_S64;
		break;
	}
	case OBJECT_TYPE_S64:
	{
		int64_t tmp;

		dbg_printf("op load field s64\n");
		tmp = *(int64_t *) stack_top->u.ptr.ptr;
		if (stack_top->u.ptr.rev_bo)
			tmp = lttng_ust_bswap_64(tmp);
		stack_top->u.v = tmp;
		stack_top->type = REG_S64;
		break;
	}
	case OBJECT_TYPE_SIGNED_ENUM:
	{
		int64_t tmp;

		dbg_printf("op load field signed enumeration\n");
		tmp = *(int64_t *) stack_top->u.ptr.ptr;
		if (stack_top->u.ptr.rev_bo)
			tmp = lttng_ust_bswap_64(tmp);
		stack_top->u.v = tmp;
		stack_top->type = REG_S64;
		break;
	}
	case OBJECT_TYPE_U8:
		dbg_printf("op load field u8\n");
		stack_top->u.v = *(uint8_t *) stack_top->u.ptr.ptr;
		stack_top->type = REG_U64;
		break;
	case OBJECT_TYPE_U16:
	{
		uint16_t tmp;

		dbg_printf("op load field u16\n");
		tmp = *(uint16_t *) stack_top->u.ptr.ptr;
		if (stack_top->u.ptr.rev_bo)
			tmp = lttng_ust_bswap_16(tmp);
		stack_top->u.v = tmp;
		stack_top->type = REG_U64;
		break;
	}
	case OBJECT_TYPE_U32:
	{
		uint32_t tmp;

		dbg_printf("op load field u32\n");
		tmp = *(uint32_t *) stack_top->u.ptr.ptr;
		if (stack_top->u.ptr.rev_bo)
			tmp = lttng_ust_bswap_32(tmp);
		stack_top->u.v = tmp;
		stack_top->type = REG_U64;
		break;
	}
	case OBJECT_TYPE_U64:
	{
		uint64_t tmp;

		dbg_printf("op load field u64\n");
		tmp = *(uint64_t *) stack_top->u.ptr.ptr;
		if (stack_top->u.ptr.rev_bo)
			tmp = lttng_ust_bswap_64(tmp);
		stack_top->u.v = tmp;
		stack_top->type = REG_U64;
		break;
	}
	case OBJECT_TYPE_UNSIGNED_ENUM:
	{
		uint64_t tmp;

		dbg_printf("op load field unsigned enumeration\n");
		tmp = *(uint64_t *) stack_top->u.ptr.ptr;
		if (stack_top->u.ptr.rev_bo)
			tmp = lttng_ust_bswap_64(tmp);
		stack_top->u.v = tmp;
		stack_top->type = REG_U64;
		break;
	}
	case OBJECT_TYPE_DOUBLE:
		memcpy(&stack_top->u.d,
			stack_top->u.ptr.ptr,
			sizeof(struct literal_double));
		stack_top->type = REG_DOUBLE;
		break;
	case OBJECT_TYPE_STRING:
	{
		const char *str;

		dbg_printf("op load field string\n");
		str = (const char *) stack_top->u.ptr.ptr;
		stack_top->u.s.str = str;
		if (unlikely(!stack_top->u.s.str)) {
			dbg_printf("Interpreter warning: loading a NULL string.\n");
			ret = -EINVAL;
			goto end;
		}
		stack_top->u.s.seq_len = SIZE_MAX;
		stack_top->u.s.literal_type =
			ESTACK_STRING_LITERAL_TYPE_NONE;
		stack_top->type = REG_STRING;
		break;
	}
	case OBJECT_TYPE_STRING_SEQUENCE:
	{
		const char *ptr;

		dbg_printf("op load field string sequence\n");
		ptr = stack_top->u.ptr.ptr;
		stack_top->u.s.seq_len = *(unsigned long *) ptr;
		stack_top->u.s.str = *(const char **) (ptr + sizeof(unsigned long));
		stack_top->type = REG_STRING;
		if (unlikely(!stack_top->u.s.str)) {
			dbg_printf("Interpreter warning: loading a NULL sequence.\n");
			ret = -EINVAL;
			goto end;
		}
		stack_top->u.s.literal_type =
			ESTACK_STRING_LITERAL_TYPE_NONE;
		break;
	}
	case OBJECT_TYPE_DYNAMIC:
		/*
		 * Dynamic types in context are looked up
		 * by context get index.
		 */
		ret = -EINVAL;
		goto end;
	case OBJECT_TYPE_SEQUENCE:
	case OBJECT_TYPE_ARRAY:
	case OBJECT_TYPE_STRUCT:
	case OBJECT_TYPE_VARIANT:
		ERR("Sequences, arrays, struct and variant cannot be loaded (nested types).");
		ret = -EINVAL;
		goto end;
	}
	return 0;

end:
	return ret;
}

static
int side_bytecode_interpret_format_output(struct estack_entry *ax,
		struct lttng_interpreter_output *output)
{
	int ret;

again:
	switch (ax->type) {
	case REG_S64:
		output->type = LTTNG_INTERPRETER_TYPE_S64;
		output->u.s = ax->u.v;
		break;
	case REG_U64:
		output->type = LTTNG_INTERPRETER_TYPE_U64;
		output->u.u = (uint64_t) ax->u.v;
		break;
	case REG_DOUBLE:
		output->type = LTTNG_INTERPRETER_TYPE_DOUBLE;
		output->u.d = ax->u.d;
		break;
	case REG_STRING:
		output->type = LTTNG_INTERPRETER_TYPE_STRING;
		output->u.str.str = ax->u.s.str;
		output->u.str.len = ax->u.s.seq_len;
		break;
	case REG_PTR:
		switch (ax->u.ptr.object_type) {
		case OBJECT_TYPE_S8:
		case OBJECT_TYPE_S16:
		case OBJECT_TYPE_S32:
		case OBJECT_TYPE_S64:
		case OBJECT_TYPE_U8:
		case OBJECT_TYPE_U16:
		case OBJECT_TYPE_U32:
		case OBJECT_TYPE_U64:
		case OBJECT_TYPE_DOUBLE:
		case OBJECT_TYPE_STRING:
		case OBJECT_TYPE_STRING_SEQUENCE:	/* Fall-through. */
		case OBJECT_TYPE_DYNAMIC:
			/*
			 * Array and VLA elements are typed by the
			 * argument itself: load it to find out.
			 */
			ret = dynamic_load_field(ax);
			if (ret)
				return ret;
			/* Retry after loading ptr into stack top. */
			goto again;
		case OBJECT_TYPE_SEQUENCE:	/* Fall-through. */
		case OBJECT_TYPE_ARRAY:
			/*
			 * Capturing a whole side array or VLA would
			 * require iterating on its nested argument
			 * vector: the capture output expects an array
			 * of elements contiguous in memory, which side
			 * arguments are not. Individual elements can be
			 * captured.
			 */
			return -EINVAL;
		case OBJECT_TYPE_SIGNED_ENUM:
			ret = dynamic_load_field(ax);
			if (ret)
				return ret;
			output->type = LTTNG_INTERPRETER_TYPE_SIGNED_ENUM;
			output->u.s = ax->u.v;
			break;
		case OBJECT_TYPE_UNSIGNED_ENUM:
			ret = dynamic_load_field(ax);
			if (ret)
				return ret;
			output->type = LTTNG_INTERPRETER_TYPE_UNSIGNED_ENUM;
			output->u.u = ax->u.v;
			break;
		case OBJECT_TYPE_STRUCT:
		case OBJECT_TYPE_VARIANT:
		default:
			return -EINVAL;
		}

		break;
	case REG_STAR_GLOB_STRING:
	case REG_UNKNOWN:
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * Return LTTNG_UST_BYTECODE_INTERPRETER_OK on success.
 * Return LTTNG_UST_BYTECODE_INTERPRETER_ERROR on error.
 *
 * For FILTER bytecode: expect a struct lttng_ust_bytecode_filter_ctx *
 * as @ctx argument.
 * For CAPTURE bytecode: expect a struct lttng_interpreter_output *
 * as @ctx argument.
 */
int lttng_bytecode_interpret_side(struct lttng_ust_bytecode_runtime *ust_bytecode,
		const char *interpreter_stack_data,
		struct lttng_ust_probe_ctx *probe_ctx,
		void *caller_ctx)
{
	struct bytecode_runtime *bytecode = caa_container_of(ust_bytecode, struct bytecode_runtime, p);
	struct lttng_ust_ctx *ctx = lttng_ust_rcu_dereference(*ust_bytecode->pctx);
	void *pc, *next_pc, *start_pc;
	int ret = -EINVAL, retval = 0;
	struct estack _stack;
	struct estack *stack = &_stack;
	register int64_t ax = 0, bx = 0;
	register enum entry_type ax_t = REG_UNKNOWN, bx_t = REG_UNKNOWN;
	register int top = INTERPRETER_STACK_EMPTY;
#ifndef INTERPRETER_USE_SWITCH
	static void *dispatch[NR_BYTECODE_OPS] = {
		[ BYTECODE_OP_UNKNOWN ] = &&LABEL_BYTECODE_OP_UNKNOWN,

		[ BYTECODE_OP_RETURN ] = &&LABEL_BYTECODE_OP_RETURN,

		/* binary */
		[ BYTECODE_OP_MUL ] = &&LABEL_BYTECODE_OP_MUL,
		[ BYTECODE_OP_DIV ] = &&LABEL_BYTECODE_OP_DIV,
		[ BYTECODE_OP_MOD ] = &&LABEL_BYTECODE_OP_MOD,
		[ BYTECODE_OP_PLUS ] = &&LABEL_BYTECODE_OP_PLUS,
		[ BYTECODE_OP_MINUS ] = &&LABEL_BYTECODE_OP_MINUS,
		[ BYTECODE_OP_BIT_RSHIFT ] = &&LABEL_BYTECODE_OP_BIT_RSHIFT,
		[ BYTECODE_OP_BIT_LSHIFT ] = &&LABEL_BYTECODE_OP_BIT_LSHIFT,
		[ BYTECODE_OP_BIT_AND ] = &&LABEL_BYTECODE_OP_BIT_AND,
		[ BYTECODE_OP_BIT_OR ] = &&LABEL_BYTECODE_OP_BIT_OR,
		[ BYTECODE_OP_BIT_XOR ] = &&LABEL_BYTECODE_OP_BIT_XOR,

		/* binary comparators */
		[ BYTECODE_OP_EQ ] = &&LABEL_BYTECODE_OP_EQ,
		[ BYTECODE_OP_NE ] = &&LABEL_BYTECODE_OP_NE,
		[ BYTECODE_OP_GT ] = &&LABEL_BYTECODE_OP_GT,
		[ BYTECODE_OP_LT ] = &&LABEL_BYTECODE_OP_LT,
		[ BYTECODE_OP_GE ] = &&LABEL_BYTECODE_OP_GE,
		[ BYTECODE_OP_LE ] = &&LABEL_BYTECODE_OP_LE,

		/* string binary comparator */
		[ BYTECODE_OP_EQ_STRING ] = &&LABEL_BYTECODE_OP_EQ_STRING,
		[ BYTECODE_OP_NE_STRING ] = &&LABEL_BYTECODE_OP_NE_STRING,
		[ BYTECODE_OP_GT_STRING ] = &&LABEL_BYTECODE_OP_GT_STRING,
		[ BYTECODE_OP_LT_STRING ] = &&LABEL_BYTECODE_OP_LT_STRING,
		[ BYTECODE_OP_GE_STRING ] = &&LABEL_BYTECODE_OP_GE_STRING,
		[ BYTECODE_OP_LE_STRING ] = &&LABEL_BYTECODE_OP_LE_STRING,

		/* globbing pattern binary comparator */
		[ BYTECODE_OP_EQ_STAR_GLOB_STRING ] = &&LABEL_BYTECODE_OP_EQ_STAR_GLOB_STRING,
		[ BYTECODE_OP_NE_STAR_GLOB_STRING ] = &&LABEL_BYTECODE_OP_NE_STAR_GLOB_STRING,

		/* s64 binary comparator */
		[ BYTECODE_OP_EQ_S64 ] = &&LABEL_BYTECODE_OP_EQ_S64,
		[ BYTECODE_OP_NE_S64 ] = &&LABEL_BYTECODE_OP_NE_S64,
		[ BYTECODE_OP_GT_S64 ] = &&LABEL_BYTECODE_OP_GT_S64,
		[ BYTECODE_OP_LT_S64 ] = &&LABEL_BYTECODE_OP_LT_S64,
		[ BYTECODE_OP_GE_S64 ] = &&LABEL_BYTECODE_OP_GE_S64,
		[ BYTECODE_OP_LE_S64 ] = &&LABEL_BYTECODE_OP_LE_S64,

		/* double binary comparator */
		[ BYTECODE_OP_EQ_DOUBLE ] = &&LABEL_BYTECODE_OP_EQ_DOUBLE,
		[ BYTECODE_OP_NE_DOUBLE ] = &&LABEL_BYTECODE_OP_NE_DOUBLE,
		[ BYTECODE_OP_GT_DOUBLE ] = &&LABEL_BYTECODE_OP_GT_DOUBLE,
		[ BYTECODE_OP_LT_DOUBLE ] = &&LABEL_BYTECODE_OP_LT_DOUBLE,
		[ BYTECODE_OP_GE_DOUBLE ] = &&LABEL_BYTECODE_OP_GE_DOUBLE,
		[ BYTECODE_OP_LE_DOUBLE ] = &&LABEL_BYTECODE_OP_LE_DOUBLE,

		/* Mixed S64-double binary comparators */
		[ BYTECODE_OP_EQ_DOUBLE_S64 ] = &&LABEL_BYTECODE_OP_EQ_DOUBLE_S64,
		[ BYTECODE_OP_NE_DOUBLE_S64 ] = &&LABEL_BYTECODE_OP_NE_DOUBLE_S64,
		[ BYTECODE_OP_GT_DOUBLE_S64 ] = &&LABEL_BYTECODE_OP_GT_DOUBLE_S64,
		[ BYTECODE_OP_LT_DOUBLE_S64 ] = &&LABEL_BYTECODE_OP_LT_DOUBLE_S64,
		[ BYTECODE_OP_GE_DOUBLE_S64 ] = &&LABEL_BYTECODE_OP_GE_DOUBLE_S64,
		[ BYTECODE_OP_LE_DOUBLE_S64 ] = &&LABEL_BYTECODE_OP_LE_DOUBLE_S64,

		[ BYTECODE_OP_EQ_S64_DOUBLE ] = &&LABEL_BYTECODE_OP_EQ_S64_DOUBLE,
		[ BYTECODE_OP_NE_S64_DOUBLE ] = &&LABEL_BYTECODE_OP_NE_S64_DOUBLE,
		[ BYTECODE_OP_GT_S64_DOUBLE ] = &&LABEL_BYTECODE_OP_GT_S64_DOUBLE,
		[ BYTECODE_OP_LT_S64_DOUBLE ] = &&LABEL_BYTECODE_OP_LT_S64_DOUBLE,
		[ BYTECODE_OP_GE_S64_DOUBLE ] = &&LABEL_BYTECODE_OP_GE_S64_DOUBLE,
		[ BYTECODE_OP_LE_S64_DOUBLE ] = &&LABEL_BYTECODE_OP_LE_S64_DOUBLE,

		/* unary */
		[ BYTECODE_OP_UNARY_PLUS ] = &&LABEL_BYTECODE_OP_UNARY_PLUS,
		[ BYTECODE_OP_UNARY_MINUS ] = &&LABEL_BYTECODE_OP_UNARY_MINUS,
		[ BYTECODE_OP_UNARY_NOT ] = &&LABEL_BYTECODE_OP_UNARY_NOT,
		[ BYTECODE_OP_UNARY_PLUS_S64 ] = &&LABEL_BYTECODE_OP_UNARY_PLUS_S64,
		[ BYTECODE_OP_UNARY_MINUS_S64 ] = &&LABEL_BYTECODE_OP_UNARY_MINUS_S64,
		[ BYTECODE_OP_UNARY_NOT_S64 ] = &&LABEL_BYTECODE_OP_UNARY_NOT_S64,
		[ BYTECODE_OP_UNARY_PLUS_DOUBLE ] = &&LABEL_BYTECODE_OP_UNARY_PLUS_DOUBLE,
		[ BYTECODE_OP_UNARY_MINUS_DOUBLE ] = &&LABEL_BYTECODE_OP_UNARY_MINUS_DOUBLE,
		[ BYTECODE_OP_UNARY_NOT_DOUBLE ] = &&LABEL_BYTECODE_OP_UNARY_NOT_DOUBLE,

		/* logical */
		[ BYTECODE_OP_AND ] = &&LABEL_BYTECODE_OP_AND,
		[ BYTECODE_OP_OR ] = &&LABEL_BYTECODE_OP_OR,

		/* load field ref */
		[ BYTECODE_OP_LOAD_FIELD_REF ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_REF,
		[ BYTECODE_OP_LOAD_FIELD_REF_STRING ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_REF_STRING,
		[ BYTECODE_OP_LOAD_FIELD_REF_SEQUENCE ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_REF_SEQUENCE,
		[ BYTECODE_OP_LOAD_FIELD_REF_S64 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_REF_S64,
		[ BYTECODE_OP_LOAD_FIELD_REF_DOUBLE ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_REF_DOUBLE,

		/* load from immediate operand */
		[ BYTECODE_OP_LOAD_STRING ] = &&LABEL_BYTECODE_OP_LOAD_STRING,
		[ BYTECODE_OP_LOAD_STAR_GLOB_STRING ] = &&LABEL_BYTECODE_OP_LOAD_STAR_GLOB_STRING,
		[ BYTECODE_OP_LOAD_S64 ] = &&LABEL_BYTECODE_OP_LOAD_S64,
		[ BYTECODE_OP_LOAD_DOUBLE ] = &&LABEL_BYTECODE_OP_LOAD_DOUBLE,

		/* cast */
		[ BYTECODE_OP_CAST_TO_S64 ] = &&LABEL_BYTECODE_OP_CAST_TO_S64,
		[ BYTECODE_OP_CAST_DOUBLE_TO_S64 ] = &&LABEL_BYTECODE_OP_CAST_DOUBLE_TO_S64,
		[ BYTECODE_OP_CAST_NOP ] = &&LABEL_BYTECODE_OP_CAST_NOP,

		/* get context ref */
		[ BYTECODE_OP_GET_CONTEXT_REF ] = &&LABEL_BYTECODE_OP_GET_CONTEXT_REF,
		[ BYTECODE_OP_GET_CONTEXT_REF_STRING ] = &&LABEL_BYTECODE_OP_GET_CONTEXT_REF_STRING,
		[ BYTECODE_OP_GET_CONTEXT_REF_S64 ] = &&LABEL_BYTECODE_OP_GET_CONTEXT_REF_S64,
		[ BYTECODE_OP_GET_CONTEXT_REF_DOUBLE ] = &&LABEL_BYTECODE_OP_GET_CONTEXT_REF_DOUBLE,

		/* Instructions for recursive traversal through composed types. */
		[ BYTECODE_OP_GET_CONTEXT_ROOT ] = &&LABEL_BYTECODE_OP_GET_CONTEXT_ROOT,
		[ BYTECODE_OP_GET_APP_CONTEXT_ROOT ] = &&LABEL_BYTECODE_OP_GET_APP_CONTEXT_ROOT,
		[ BYTECODE_OP_GET_PAYLOAD_ROOT ] = &&LABEL_BYTECODE_OP_GET_PAYLOAD_ROOT,

		[ BYTECODE_OP_GET_SYMBOL ] = &&LABEL_BYTECODE_OP_GET_SYMBOL,
		[ BYTECODE_OP_GET_SYMBOL_FIELD ] = &&LABEL_BYTECODE_OP_GET_SYMBOL_FIELD,
		[ BYTECODE_OP_GET_INDEX_U16 ] = &&LABEL_BYTECODE_OP_GET_INDEX_U16,
		[ BYTECODE_OP_GET_INDEX_U64 ] = &&LABEL_BYTECODE_OP_GET_INDEX_U64,

		[ BYTECODE_OP_LOAD_FIELD ] = &&LABEL_BYTECODE_OP_LOAD_FIELD,
		[ BYTECODE_OP_LOAD_FIELD_S8	 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_S8,
		[ BYTECODE_OP_LOAD_FIELD_S16 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_S16,
		[ BYTECODE_OP_LOAD_FIELD_S32 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_S32,
		[ BYTECODE_OP_LOAD_FIELD_S64 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_S64,
		[ BYTECODE_OP_LOAD_FIELD_U8 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_U8,
		[ BYTECODE_OP_LOAD_FIELD_U16 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_U16,
		[ BYTECODE_OP_LOAD_FIELD_U32 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_U32,
		[ BYTECODE_OP_LOAD_FIELD_U64 ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_U64,
		[ BYTECODE_OP_LOAD_FIELD_STRING ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_STRING,
		[ BYTECODE_OP_LOAD_FIELD_SEQUENCE ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_SEQUENCE,
		[ BYTECODE_OP_LOAD_FIELD_DOUBLE ] = &&LABEL_BYTECODE_OP_LOAD_FIELD_DOUBLE,

		[ BYTECODE_OP_UNARY_BIT_NOT ] = &&LABEL_BYTECODE_OP_UNARY_BIT_NOT,

		[ BYTECODE_OP_RETURN_S64 ] = &&LABEL_BYTECODE_OP_RETURN_S64,
	};
#endif /* #ifndef INTERPRETER_USE_SWITCH */

	START_OP

		OP(BYTECODE_OP_UNKNOWN):
		OP(BYTECODE_OP_LOAD_FIELD_REF):
#ifdef INTERPRETER_USE_SWITCH
		default:
#endif /* INTERPRETER_USE_SWITCH */
			ERR("unknown bytecode op %u",
				(unsigned int) *(bytecode_opcode_t *) pc);
			ret = -EINVAL;
			goto end;

		OP(BYTECODE_OP_RETURN):
			/* LTTNG_UST_BYTECODE_INTERPRETER_ERROR or LTTNG_UST_BYTECODE_INTERPRETER_OK */
			/* Handle dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:
			case REG_U64:
				retval = !!estack_ax_v;
				break;
			case REG_DOUBLE:
			case REG_STRING:
			case REG_PTR:
				if (ust_bytecode->type != LTTNG_UST_BYTECODE_TYPE_CAPTURE) {
					ret = -EINVAL;
					goto end;
				}
				retval = 0;
				break;
			case REG_STAR_GLOB_STRING:
			case REG_UNKNOWN:
			default:
				ret = -EINVAL;
				goto end;
			}
			ret = 0;
			goto end;

		OP(BYTECODE_OP_RETURN_S64):
			/* LTTNG_UST_BYTECODE_INTERPRETER_ERROR or LTTNG_UST_BYTECODE_INTERPRETER_OK */
			retval = !!estack_ax_v;
			ret = 0;
			goto end;

		/* binary */
		OP(BYTECODE_OP_MUL):
		OP(BYTECODE_OP_DIV):
		OP(BYTECODE_OP_MOD):
		OP(BYTECODE_OP_PLUS):
		OP(BYTECODE_OP_MINUS):
			ERR("unsupported bytecode op %u",
				(unsigned int) *(bytecode_opcode_t *) pc);
			ret = -EINVAL;
			goto end;

		OP(BYTECODE_OP_EQ):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through */
			case REG_U64:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_EQ_S64);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_EQ_DOUBLE_S64);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_DOUBLE:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_EQ_S64_DOUBLE);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_EQ_DOUBLE);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_STRING:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:	/* Fall-through */
				case REG_DOUBLE:
					ret = -EINVAL;
					goto end;
				case REG_STRING:
					JUMP_TO(BYTECODE_OP_EQ_STRING);
				case REG_STAR_GLOB_STRING:
					JUMP_TO(BYTECODE_OP_EQ_STAR_GLOB_STRING);
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_STAR_GLOB_STRING:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:	/* Fall-through */
				case REG_DOUBLE:
					ret = -EINVAL;
					goto end;
				case REG_STRING:
					JUMP_TO(BYTECODE_OP_EQ_STAR_GLOB_STRING);
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}
		OP(BYTECODE_OP_NE):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through */
			case REG_U64:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_NE_S64);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_NE_DOUBLE_S64);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_DOUBLE:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_NE_S64_DOUBLE);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_NE_DOUBLE);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_STRING:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
				case REG_DOUBLE:
					ret = -EINVAL;
					goto end;
				case REG_STRING:
					JUMP_TO(BYTECODE_OP_NE_STRING);
				case REG_STAR_GLOB_STRING:
					JUMP_TO(BYTECODE_OP_NE_STAR_GLOB_STRING);
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_STAR_GLOB_STRING:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
				case REG_DOUBLE:
					ret = -EINVAL;
					goto end;
				case REG_STRING:
					JUMP_TO(BYTECODE_OP_NE_STAR_GLOB_STRING);
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}
		OP(BYTECODE_OP_GT):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through */
			case REG_U64:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_GT_S64);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_GT_DOUBLE_S64);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_DOUBLE:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_GT_S64_DOUBLE);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_GT_DOUBLE);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_STRING:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:	/* Fall-through */
				case REG_DOUBLE: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				case REG_STRING:
					JUMP_TO(BYTECODE_OP_GT_STRING);
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}
		OP(BYTECODE_OP_LT):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through */
			case REG_U64:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_LT_S64);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_LT_DOUBLE_S64);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_DOUBLE:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_LT_S64_DOUBLE);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_LT_DOUBLE);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_STRING:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:	/* Fall-through */
				case REG_DOUBLE: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				case REG_STRING:
					JUMP_TO(BYTECODE_OP_LT_STRING);
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}
		OP(BYTECODE_OP_GE):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through */
			case REG_U64:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_GE_S64);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_GE_DOUBLE_S64);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_DOUBLE:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_GE_S64_DOUBLE);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_GE_DOUBLE);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_STRING:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:	/* Fall-through */
				case REG_DOUBLE: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				case REG_STRING:
					JUMP_TO(BYTECODE_OP_GE_STRING);
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}
		OP(BYTECODE_OP_LE):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through */
			case REG_U64:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_LE_S64);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_LE_DOUBLE_S64);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_DOUBLE:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:
					JUMP_TO(BYTECODE_OP_LE_S64_DOUBLE);
				case REG_DOUBLE:
					JUMP_TO(BYTECODE_OP_LE_DOUBLE);
				case REG_STRING: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			case REG_STRING:
				switch (estack_bx_t) {
				case REG_S64:	/* Fall-through */
				case REG_U64:	/* Fall-through */
				case REG_DOUBLE: /* Fall-through */
				case REG_STAR_GLOB_STRING:
					ret = -EINVAL;
					goto end;
				case REG_STRING:
					JUMP_TO(BYTECODE_OP_LE_STRING);
				default:
					ERR("Unknown interpreter register type (%d)",
						(int) estack_bx_t);
					ret = -EINVAL;
					goto end;
				}
				break;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}

		OP(BYTECODE_OP_EQ_STRING):
		{
			int res;

			res = (stack_strcmp(stack, top, "==") == 0);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_NE_STRING):
		{
			int res;

			res = (stack_strcmp(stack, top, "!=") != 0);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GT_STRING):
		{
			int res;

			res = (stack_strcmp(stack, top, ">") > 0);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LT_STRING):
		{
			int res;

			res = (stack_strcmp(stack, top, "<") < 0);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GE_STRING):
		{
			int res;

			res = (stack_strcmp(stack, top, ">=") >= 0);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LE_STRING):
		{
			int res;

			res = (stack_strcmp(stack, top, "<=") <= 0);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}

		OP(BYTECODE_OP_EQ_STAR_GLOB_STRING):
		{
			int res;

			res = (stack_star_glob_match(stack, top, "==") == 0);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_NE_STAR_GLOB_STRING):
		{
			int res;

			res = (stack_star_glob_match(stack, top, "!=") != 0);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}

		OP(BYTECODE_OP_EQ_S64):
		{
			int res;

			res = (estack_bx_v == estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_NE_S64):
		{
			int res;

			res = (estack_bx_v != estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GT_S64):
		{
			int res;

			res = (estack_bx_v > estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LT_S64):
		{
			int res;

			res = (estack_bx_v < estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GE_S64):
		{
			int res;

			res = (estack_bx_v >= estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LE_S64):
		{
			int res;

			res = (estack_bx_v <= estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}

		OP(BYTECODE_OP_EQ_DOUBLE):
		{
			int res;

			res = (estack_bx(stack, top)->u.d == estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_NE_DOUBLE):
		{
			int res;

			res = (estack_bx(stack, top)->u.d != estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GT_DOUBLE):
		{
			int res;

			res = (estack_bx(stack, top)->u.d > estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LT_DOUBLE):
		{
			int res;

			res = (estack_bx(stack, top)->u.d < estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GE_DOUBLE):
		{
			int res;

			res = (estack_bx(stack, top)->u.d >= estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LE_DOUBLE):
		{
			int res;

			res = (estack_bx(stack, top)->u.d <= estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}

		/* Mixed S64-double binary comparators */
		OP(BYTECODE_OP_EQ_DOUBLE_S64):
		{
			int res;

			res = (estack_bx(stack, top)->u.d == estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_NE_DOUBLE_S64):
		{
			int res;

			res = (estack_bx(stack, top)->u.d != estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GT_DOUBLE_S64):
		{
			int res;

			res = (estack_bx(stack, top)->u.d > estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LT_DOUBLE_S64):
		{
			int res;

			res = (estack_bx(stack, top)->u.d < estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GE_DOUBLE_S64):
		{
			int res;

			res = (estack_bx(stack, top)->u.d >= estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LE_DOUBLE_S64):
		{
			int res;

			res = (estack_bx(stack, top)->u.d <= estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}

		OP(BYTECODE_OP_EQ_S64_DOUBLE):
		{
			int res;

			res = (estack_bx_v == estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_NE_S64_DOUBLE):
		{
			int res;

			res = (estack_bx_v != estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GT_S64_DOUBLE):
		{
			int res;

			res = (estack_bx_v > estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LT_S64_DOUBLE):
		{
			int res;

			res = (estack_bx_v < estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_GE_S64_DOUBLE):
		{
			int res;

			res = (estack_bx_v >= estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_LE_S64_DOUBLE):
		{
			int res;

			res = (estack_bx_v <= estack_ax(stack, top)->u.d);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_BIT_RSHIFT):
		{
			int64_t res;

			if (!IS_INTEGER_REGISTER(estack_ax_t) || !IS_INTEGER_REGISTER(estack_bx_t)) {
				ret = -EINVAL;
				goto end;
			}

			/* Catch undefined behavior. */
			if (caa_unlikely(estack_ax_v < 0 || estack_ax_v >= 64)) {
				ret = -EINVAL;
				goto end;
			}
			res = ((uint64_t) estack_bx_v >> (uint32_t) estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_BIT_LSHIFT):
		{
			int64_t res;

			if (!IS_INTEGER_REGISTER(estack_ax_t) || !IS_INTEGER_REGISTER(estack_bx_t)) {
				ret = -EINVAL;
				goto end;
			}

			/* Catch undefined behavior. */
			if (caa_unlikely(estack_ax_v < 0 || estack_ax_v >= 64)) {
				ret = -EINVAL;
				goto end;
			}
			res = ((uint64_t) estack_bx_v << (uint32_t) estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_BIT_AND):
		{
			int64_t res;

			if (!IS_INTEGER_REGISTER(estack_ax_t) || !IS_INTEGER_REGISTER(estack_bx_t)) {
				ret = -EINVAL;
				goto end;
			}

			res = ((uint64_t) estack_bx_v & (uint64_t) estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_BIT_OR):
		{
			int64_t res;

			if (!IS_INTEGER_REGISTER(estack_ax_t) || !IS_INTEGER_REGISTER(estack_bx_t)) {
				ret = -EINVAL;
				goto end;
			}

			res = ((uint64_t) estack_bx_v | (uint64_t) estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct binary_op);
			PO;
		}
		OP(BYTECODE_OP_BIT_XOR):
		{
			int64_t res;

			if (!IS_INTEGER_REGISTER(estack_ax_t) || !IS_INTEGER_REGISTER(estack_bx_t)) {
				ret = -EINVAL;
				goto end;
			}

			res = ((uint64_t) estack_bx_v ^ (uint64_t) estack_ax_v);
			estack_pop(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = res;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct binary_op);
			PO;
		}

		/* unary */
		OP(BYTECODE_OP_UNARY_PLUS):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through. */
			case REG_U64:
				JUMP_TO(BYTECODE_OP_UNARY_PLUS_S64);
			case REG_DOUBLE:
				JUMP_TO(BYTECODE_OP_UNARY_PLUS_DOUBLE);
			case REG_STRING: /* Fall-through */
			case REG_STAR_GLOB_STRING:
				ret = -EINVAL;
				goto end;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}
		OP(BYTECODE_OP_UNARY_MINUS):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through. */
			case REG_U64:
				JUMP_TO(BYTECODE_OP_UNARY_MINUS_S64);
			case REG_DOUBLE:
				JUMP_TO(BYTECODE_OP_UNARY_MINUS_DOUBLE);
			case REG_STRING: /* Fall-through */
			case REG_STAR_GLOB_STRING:
				ret = -EINVAL;
				goto end;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}
		OP(BYTECODE_OP_UNARY_NOT):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:	/* Fall-through. */
			case REG_U64:
				JUMP_TO(BYTECODE_OP_UNARY_NOT_S64);
			case REG_DOUBLE:
				JUMP_TO(BYTECODE_OP_UNARY_NOT_DOUBLE);
			case REG_STRING: /* Fall-through */
			case REG_STAR_GLOB_STRING:
				ret = -EINVAL;
				goto end;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
			next_pc += sizeof(struct unary_op);
			PO;
		}

		OP(BYTECODE_OP_UNARY_BIT_NOT):
		{
			/* Dynamic typing. */
			if (!IS_INTEGER_REGISTER(estack_ax_t)) {
				ret = -EINVAL;
				goto end;
			}

			estack_ax_v = ~(uint64_t) estack_ax_v;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct unary_op);
			PO;
		}

		OP(BYTECODE_OP_UNARY_PLUS_S64):
		OP(BYTECODE_OP_UNARY_PLUS_DOUBLE):
		{
			next_pc += sizeof(struct unary_op);
			PO;
		}
		OP(BYTECODE_OP_UNARY_MINUS_S64):
		{
			estack_ax_v = -estack_ax_v;
			next_pc += sizeof(struct unary_op);
			PO;
		}
		OP(BYTECODE_OP_UNARY_MINUS_DOUBLE):
		{
			estack_ax(stack, top)->u.d = -estack_ax(stack, top)->u.d;
			next_pc += sizeof(struct unary_op);
			PO;
		}
		OP(BYTECODE_OP_UNARY_NOT_S64):
		{
			estack_ax_v = !estack_ax_v;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct unary_op);
			PO;
		}
		OP(BYTECODE_OP_UNARY_NOT_DOUBLE):
		{
			estack_ax_v = !estack_ax(stack, top)->u.d;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct unary_op);
			PO;
		}

		/* logical */
		OP(BYTECODE_OP_AND):
		{
			struct logical_op *insn = (struct logical_op *) pc;

			if (estack_ax_t != REG_S64 && estack_ax_t != REG_U64) {
				ret = -EINVAL;
				goto end;
			}
			/* If AX is 0, skip and evaluate to 0 */
			if (unlikely(estack_ax_v == 0)) {
				dbg_printf("Jumping to bytecode offset %u\n",
					(unsigned int) insn->skip_offset);
				next_pc = start_pc + insn->skip_offset;
			} else {
				/* Pop 1 when jump not taken */
				estack_pop(stack, top, ax, bx, ax_t, bx_t);
				next_pc += sizeof(struct logical_op);
			}
			PO;
		}
		OP(BYTECODE_OP_OR):
		{
			struct logical_op *insn = (struct logical_op *) pc;

			if (estack_ax_t != REG_S64 && estack_ax_t != REG_U64) {
				ret = -EINVAL;
				goto end;
			}
			/* If AX is nonzero, skip and evaluate to 1 */
			if (unlikely(estack_ax_v != 0)) {
				estack_ax_v = 1;
				dbg_printf("Jumping to bytecode offset %u\n",
					(unsigned int) insn->skip_offset);
				next_pc = start_pc + insn->skip_offset;
			} else {
				/* Pop 1 when jump not taken */
				estack_pop(stack, top, ax, bx, ax_t, bx_t);
				next_pc += sizeof(struct logical_op);
			}
			PO;
		}


		/* load field ref */
		OP(BYTECODE_OP_LOAD_FIELD_REF_STRING):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct field_ref *ref = (struct field_ref *) insn->data;
			const struct side_arg_vec *sav =
				(const struct side_arg_vec *) interpreter_stack_data;
			const char *str;

			dbg_printf("load field ref path at %u type string\n",
				ref->offset);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			{
				struct side_field_ref fref;

				ret = lttng_bytecode_side_field_ref(bytecode->side_desc,
					(const struct side_field_path *)
						&bytecode->data[ref->offset],
					sav, &fref);
				if (unlikely(ret))
					goto end;
				ret = side_gather_load_field_string(fref.type,
					fref.gather_base, &str);
				if (ret == -ENOENT) {
					if (unlikely(!fref.arg)) {
						ret = -EINVAL;
						goto end;
					}
					ret = side_arg_load_string(fref.arg, &str);
				}
			}
			if (unlikely(ret))
				goto end;
			if (unlikely(!str)) {
				dbg_printf("Interpreter warning: loading a NULL string.\n");
				ret = -EINVAL;
				goto end;
			}
			estack_ax(stack, top)->u.s.str = str;
			estack_ax(stack, top)->u.s.seq_len = SIZE_MAX;
			estack_ax(stack, top)->u.s.literal_type =
				ESTACK_STRING_LITERAL_TYPE_NONE;
			estack_ax_t = REG_STRING;
			dbg_printf("ref load string %s\n", estack_ax(stack, top)->u.s.str);
			next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
			PO;
		}

		OP(BYTECODE_OP_LOAD_FIELD_REF_SEQUENCE):
		{
			/*
			 * Arrays/VLAs of integers have no string
			 * encoding: not usable in filters. The side
			 * linker never emits this instruction.
			 */
			ret = -EINVAL;
			goto end;
		}

		OP(BYTECODE_OP_LOAD_FIELD_REF_S64):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct field_ref *ref = (struct field_ref *) insn->data;
			const struct side_arg_vec *sav =
				(const struct side_arg_vec *) interpreter_stack_data;
			int64_t v;

			dbg_printf("load field ref path at %u type s64\n",
				ref->offset);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			/*
			 * The operand of a legacy field reference is
			 * where the linker resolved the path of the
			 * field, which the description this bytecode is
			 * linked against turns into a value.
			 */
			{
				struct side_field_ref fref;
				bool rev_bo;

				ret = lttng_bytecode_side_field_ref(bytecode->side_desc,
					(const struct side_field_path *)
						&bytecode->data[ref->offset],
					sav, &fref);
				if (unlikely(ret))
					goto end;
				rev_bo = lttng_bytecode_side_type_rev_bo(fref.type);
				ret = side_gather_load_field_integer(fref.type,
					fref.gather_base, rev_bo, &v);
				if (ret == -ENOENT) {
					if (unlikely(!fref.arg)) {
						ret = -EINVAL;
						goto end;
					}
					ret = side_arg_load_integer(fref.arg, rev_bo, &v);
				}
			}
			if (unlikely(ret))
				goto end;
			estack_ax_v = v;
			estack_ax_t = REG_S64;
			dbg_printf("ref load s64 %" PRIi64 "\n", estack_ax_v);
			next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
			PO;
		}

		OP(BYTECODE_OP_LOAD_FIELD_REF_DOUBLE):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct field_ref *ref = (struct field_ref *) insn->data;
			const struct side_arg_vec *sav =
				(const struct side_arg_vec *) interpreter_stack_data;
			double d;

			dbg_printf("load field ref path at %u type double\n",
				ref->offset);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			{
				struct side_field_ref fref;
				bool rev_bo;

				ret = lttng_bytecode_side_field_ref(bytecode->side_desc,
					(const struct side_field_path *)
						&bytecode->data[ref->offset],
					sav, &fref);
				if (unlikely(ret))
					goto end;
				rev_bo = lttng_bytecode_side_type_rev_bo(fref.type);
				ret = side_gather_load_field_double(fref.type,
					fref.gather_base, rev_bo, &d);
				if (ret == -ENOENT) {
					if (unlikely(!fref.arg)) {
						ret = -EINVAL;
						goto end;
					}
					ret = side_arg_load_double(fref.arg, rev_bo, &d);
				}
			}
			if (unlikely(ret))
				goto end;
			estack_ax(stack, top)->u.d = d;
			estack_ax_t = REG_DOUBLE;
			dbg_printf("ref load double %g\n", estack_ax(stack, top)->u.d);
			next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
			PO;
		}

		/* load from immediate operand */
		OP(BYTECODE_OP_LOAD_STRING):
		{
			struct load_op *insn = (struct load_op *) pc;

			dbg_printf("load string %s\n", insn->data);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			estack_ax(stack, top)->u.s.str = insn->data;
			estack_ax(stack, top)->u.s.seq_len = SIZE_MAX;
			estack_ax(stack, top)->u.s.literal_type =
				ESTACK_STRING_LITERAL_TYPE_PLAIN;
			estack_ax_t = REG_STRING;
			next_pc += sizeof(struct load_op) + strlen(insn->data) + 1;
			PO;
		}

		OP(BYTECODE_OP_LOAD_STAR_GLOB_STRING):
		{
			struct load_op *insn = (struct load_op *) pc;

			dbg_printf("load globbing pattern %s\n", insn->data);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			estack_ax(stack, top)->u.s.str = insn->data;
			estack_ax(stack, top)->u.s.seq_len = SIZE_MAX;
			estack_ax(stack, top)->u.s.literal_type =
				ESTACK_STRING_LITERAL_TYPE_STAR_GLOB;
			estack_ax_t = REG_STAR_GLOB_STRING;
			next_pc += sizeof(struct load_op) + strlen(insn->data) + 1;
			PO;
		}

		OP(BYTECODE_OP_LOAD_S64):
		{
			struct load_op *insn = (struct load_op *) pc;

			estack_push(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = ((struct literal_numeric *) insn->data)->v;
			estack_ax_t = REG_S64;
			dbg_printf("load s64 %" PRIi64 "\n", estack_ax_v);
			next_pc += sizeof(struct load_op)
					+ sizeof(struct literal_numeric);
			PO;
		}

		OP(BYTECODE_OP_LOAD_DOUBLE):
		{
			struct load_op *insn = (struct load_op *) pc;

			estack_push(stack, top, ax, bx, ax_t, bx_t);
			memcpy(&estack_ax(stack, top)->u.d, insn->data,
				sizeof(struct literal_double));
			estack_ax_t = REG_DOUBLE;
			dbg_printf("load double %g\n", estack_ax(stack, top)->u.d);
			next_pc += sizeof(struct load_op)
					+ sizeof(struct literal_double);
			PO;
		}

		/* cast */
		OP(BYTECODE_OP_CAST_TO_S64):
		{
			/* Dynamic typing. */
			switch (estack_ax_t) {
			case REG_S64:
				JUMP_TO(BYTECODE_OP_CAST_NOP);
			case REG_DOUBLE:
				JUMP_TO(BYTECODE_OP_CAST_DOUBLE_TO_S64);
			case REG_U64:
				estack_ax_t = REG_S64;
				next_pc += sizeof(struct cast_op); /* Fall-through */
			case REG_STRING: /* Fall-through */
			case REG_STAR_GLOB_STRING:
				ret = -EINVAL;
				goto end;
			default:
				ERR("Unknown interpreter register type (%d)",
					(int) estack_ax_t);
				ret = -EINVAL;
				goto end;
			}
		}

		OP(BYTECODE_OP_CAST_DOUBLE_TO_S64):
		{
			estack_ax_v = (int64_t) estack_ax(stack, top)->u.d;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct cast_op);
			PO;
		}

		OP(BYTECODE_OP_CAST_NOP):
		{
			next_pc += sizeof(struct cast_op);
			PO;
		}

		/* get context ref */
		OP(BYTECODE_OP_GET_CONTEXT_REF):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct field_ref *ref = (struct field_ref *) insn->data;
			const struct lttng_ust_ctx_field *ctx_field;
			struct lttng_ust_ctx_value v;

			dbg_printf("get context ref offset %u type dynamic\n",
				ref->offset);
			ctx_field = &ctx->fields[ref->offset];
			ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			switch (v.sel) {
			case LTTNG_UST_DYNAMIC_TYPE_NONE:
				ret = -EINVAL;
				goto end;
			case LTTNG_UST_DYNAMIC_TYPE_S64:
				estack_ax_v = v.u.s64;
				estack_ax_t = REG_S64;
				dbg_printf("ref get context dynamic s64 %" PRIi64 "\n", estack_ax_v);
				break;
			case LTTNG_UST_DYNAMIC_TYPE_DOUBLE:
				estack_ax(stack, top)->u.d = v.u.d;
				estack_ax_t = REG_DOUBLE;
				dbg_printf("ref get context dynamic double %g\n", estack_ax(stack, top)->u.d);
				break;
			case LTTNG_UST_DYNAMIC_TYPE_STRING:
				estack_ax(stack, top)->u.s.str = v.u.str;
				if (unlikely(!estack_ax(stack, top)->u.s.str)) {
					dbg_printf("Interpreter warning: loading a NULL string.\n");
					ret = -EINVAL;
					goto end;
				}
				estack_ax(stack, top)->u.s.seq_len = SIZE_MAX;
				estack_ax(stack, top)->u.s.literal_type =
					ESTACK_STRING_LITERAL_TYPE_NONE;
				dbg_printf("ref get context dynamic string %s\n", estack_ax(stack, top)->u.s.str);
				estack_ax_t = REG_STRING;
				break;
			default:
				dbg_printf("Interpreter warning: unknown dynamic type (%d).\n", (int) v.sel);
				ret = -EINVAL;
				goto end;
			}
			next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
			PO;
		}

		OP(BYTECODE_OP_GET_CONTEXT_REF_STRING):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct field_ref *ref = (struct field_ref *) insn->data;
			const struct lttng_ust_ctx_field *ctx_field;
			struct lttng_ust_ctx_value v;

			dbg_printf("get context ref offset %u type string\n",
				ref->offset);
			ctx_field = &ctx->fields[ref->offset];
			ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			estack_ax(stack, top)->u.s.str = v.u.str;
			if (unlikely(!estack_ax(stack, top)->u.s.str)) {
				dbg_printf("Interpreter warning: loading a NULL string.\n");
				ret = -EINVAL;
				goto end;
			}
			estack_ax(stack, top)->u.s.seq_len = SIZE_MAX;
			estack_ax(stack, top)->u.s.literal_type =
				ESTACK_STRING_LITERAL_TYPE_NONE;
			estack_ax_t = REG_STRING;
			dbg_printf("ref get context string %s\n", estack_ax(stack, top)->u.s.str);
			next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
			PO;
		}

		OP(BYTECODE_OP_GET_CONTEXT_REF_S64):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct field_ref *ref = (struct field_ref *) insn->data;
			const struct lttng_ust_ctx_field *ctx_field;
			struct lttng_ust_ctx_value v;

			dbg_printf("get context ref offset %u type s64\n",
				ref->offset);
			ctx_field = &ctx->fields[ref->offset];
			ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			estack_ax_v = v.u.s64;
			estack_ax_t = REG_S64;
			dbg_printf("ref get context s64 %" PRIi64 "\n", estack_ax_v);
			next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
			PO;
		}

		OP(BYTECODE_OP_GET_CONTEXT_REF_DOUBLE):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct field_ref *ref = (struct field_ref *) insn->data;
			const struct lttng_ust_ctx_field *ctx_field;
			struct lttng_ust_ctx_value v;

			dbg_printf("get context ref offset %u type double\n",
				ref->offset);
			ctx_field = &ctx->fields[ref->offset];
			ctx_field->get_value(ctx_field->priv, probe_ctx, &v);
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			memcpy(&estack_ax(stack, top)->u.d, &v.u.d, sizeof(struct literal_double));
			estack_ax_t = REG_DOUBLE;
			dbg_printf("ref get context double %g\n", estack_ax(stack, top)->u.d);
			next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
			PO;
		}

		OP(BYTECODE_OP_GET_CONTEXT_ROOT):
		{
			dbg_printf("op get context root\n");
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			estack_ax(stack, top)->u.ptr.type = LOAD_ROOT_CONTEXT;
			/* "field" only needed for variants. */
			estack_ax(stack, top)->u.ptr.field = NULL;
			estack_ax_t = REG_PTR;
			next_pc += sizeof(struct load_op);
			PO;
		}

		OP(BYTECODE_OP_GET_APP_CONTEXT_ROOT):
		{
			dbg_printf("op get app context root\n");
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			estack_ax(stack, top)->u.ptr.type = LOAD_ROOT_APP_CONTEXT;
			/* "field" only needed for variants. */
			estack_ax(stack, top)->u.ptr.field = NULL;
			estack_ax_t = REG_PTR;
			next_pc += sizeof(struct load_op);
			PO;
		}

		OP(BYTECODE_OP_GET_PAYLOAD_ROOT):
		{
			dbg_printf("op get app payload root\n");
			estack_push(stack, top, ax, bx, ax_t, bx_t);
			estack_ax(stack, top)->u.ptr.type = LOAD_ROOT_PAYLOAD;
			estack_ax(stack, top)->u.ptr.ptr = interpreter_stack_data;
			/* "field" only needed for variants. */
			estack_ax(stack, top)->u.ptr.field = NULL;
			estack_ax_t = REG_PTR;
			next_pc += sizeof(struct load_op);
			PO;
		}

		OP(BYTECODE_OP_GET_SYMBOL):
		{
			dbg_printf("op get symbol\n");
			switch (estack_ax(stack, top)->u.ptr.type) {
			case LOAD_OBJECT:
				ERR("Nested fields not implemented yet.");
				ret = -EINVAL;
				goto end;
			case LOAD_ROOT_CONTEXT:
			case LOAD_ROOT_APP_CONTEXT:
			case LOAD_ROOT_PAYLOAD:
				/*
				 * symbol lookup is performed by
				 * specialization.
				 */
				ret = -EINVAL;
				goto end;
			}
			next_pc += sizeof(struct load_op) + sizeof(struct get_symbol);
			PO;
		}

		OP(BYTECODE_OP_GET_SYMBOL_FIELD):
		{
			/*
			 * Used for first variant encountered in a
			 * traversal. Variants are not implemented yet.
			 */
			ret = -EINVAL;
			goto end;
		}

		OP(BYTECODE_OP_GET_INDEX_U16):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct get_index_u16 *index = (struct get_index_u16 *) insn->data;

			dbg_printf("op get index u16\n");
			ret = dynamic_get_index(ctx, probe_ctx, bytecode, index->index, estack_ax(stack, top));
			if (ret)
				goto end;
			estack_ax_v = estack_ax(stack, top)->u.v;
			estack_ax_t = estack_ax(stack, top)->type;
			next_pc += sizeof(struct load_op) + sizeof(struct get_index_u16);
			PO;
		}

		OP(BYTECODE_OP_GET_INDEX_U64):
		{
			struct load_op *insn = (struct load_op *) pc;
			struct get_index_u64 *index = (struct get_index_u64 *) insn->data;

			dbg_printf("op get index u64\n");
			ret = dynamic_get_index(ctx, probe_ctx, bytecode, index->index, estack_ax(stack, top));
			if (ret)
				goto end;
			estack_ax_v = estack_ax(stack, top)->u.v;
			estack_ax_t = estack_ax(stack, top)->type;
			next_pc += sizeof(struct load_op) + sizeof(struct get_index_u64);
			PO;
		}

		OP(BYTECODE_OP_LOAD_FIELD):
		{
			dbg_printf("op load field\n");
			ret = dynamic_load_field(estack_ax(stack, top));
			if (ret)
				goto end;
			estack_ax_v = estack_ax(stack, top)->u.v;
			estack_ax_t = estack_ax(stack, top)->type;
			next_pc += sizeof(struct load_op);
			PO;
		}

		OP(BYTECODE_OP_LOAD_FIELD_S8):
		{
			dbg_printf("op load field s8\n");

			estack_ax_v = *(int8_t *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct load_op);
			PO;
		}
		OP(BYTECODE_OP_LOAD_FIELD_S16):
		{
			dbg_printf("op load field s16\n");

			estack_ax_v = *(int16_t *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct load_op);
			PO;
		}
		OP(BYTECODE_OP_LOAD_FIELD_S32):
		{
			dbg_printf("op load field s32\n");

			estack_ax_v = *(int32_t *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct load_op);
			PO;
		}
		OP(BYTECODE_OP_LOAD_FIELD_S64):
		{
			dbg_printf("op load field s64\n");

			estack_ax_v = *(int64_t *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax_t = REG_S64;
			next_pc += sizeof(struct load_op);
			PO;
		}
		OP(BYTECODE_OP_LOAD_FIELD_U8):
		{
			dbg_printf("op load field u8\n");

			estack_ax_v = *(uint8_t *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct load_op);
			PO;
		}
		OP(BYTECODE_OP_LOAD_FIELD_U16):
		{
			dbg_printf("op load field u16\n");

			estack_ax_v = *(uint16_t *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct load_op);
			PO;
		}
		OP(BYTECODE_OP_LOAD_FIELD_U32):
		{
			dbg_printf("op load field u32\n");

			estack_ax_v = *(uint32_t *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct load_op);
			PO;
		}
		OP(BYTECODE_OP_LOAD_FIELD_U64):
		{
			dbg_printf("op load field u64\n");

			estack_ax_v = *(uint64_t *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax_t = REG_U64;
			next_pc += sizeof(struct load_op);
			PO;
		}
		OP(BYTECODE_OP_LOAD_FIELD_DOUBLE):
		{
			dbg_printf("op load field double\n");

			memcpy(&estack_ax(stack, top)->u.d,
				estack_ax(stack, top)->u.ptr.ptr,
				sizeof(struct literal_double));
			estack_ax(stack, top)->type = REG_DOUBLE;
			next_pc += sizeof(struct load_op);
			PO;
		}

		OP(BYTECODE_OP_LOAD_FIELD_STRING):
		{
			const char *str;

			dbg_printf("op load field string\n");
			str = (const char *) estack_ax(stack, top)->u.ptr.ptr;
			estack_ax(stack, top)->u.s.str = str;
			if (unlikely(!estack_ax(stack, top)->u.s.str)) {
				dbg_printf("Interpreter warning: loading a NULL string.\n");
				ret = -EINVAL;
				goto end;
			}
			estack_ax(stack, top)->u.s.seq_len = SIZE_MAX;
			estack_ax(stack, top)->u.s.literal_type =
				ESTACK_STRING_LITERAL_TYPE_NONE;
			estack_ax(stack, top)->type = REG_STRING;
			next_pc += sizeof(struct load_op);
			PO;
		}

		OP(BYTECODE_OP_LOAD_FIELD_SEQUENCE):
		{
			const char *ptr;

			dbg_printf("op load field string sequence\n");
			ptr = estack_ax(stack, top)->u.ptr.ptr;
			estack_ax(stack, top)->u.s.seq_len = *(unsigned long *) ptr;
			estack_ax(stack, top)->u.s.str = *(const char **) (ptr + sizeof(unsigned long));
			estack_ax(stack, top)->type = REG_STRING;
			if (unlikely(!estack_ax(stack, top)->u.s.str)) {
				dbg_printf("Interpreter warning: loading a NULL sequence.\n");
				ret = -EINVAL;
				goto end;
			}
			estack_ax(stack, top)->u.s.literal_type =
				ESTACK_STRING_LITERAL_TYPE_NONE;
			next_pc += sizeof(struct load_op);
			PO;
		}

	END_OP
end:
	/* No need to prepare output if an error occurred. */
	if (ret)
		return LTTNG_UST_BYTECODE_INTERPRETER_ERROR;

	/* Prepare output. */
	switch (ust_bytecode->type) {
	case LTTNG_UST_BYTECODE_TYPE_FILTER:
	{
		struct lttng_ust_bytecode_filter_ctx *filter_ctx =
			(struct lttng_ust_bytecode_filter_ctx *) caller_ctx;
		if (retval)
			filter_ctx->result = LTTNG_UST_BYTECODE_FILTER_ACCEPT;
		else
			filter_ctx->result = LTTNG_UST_BYTECODE_FILTER_REJECT;
		break;
	}
	case LTTNG_UST_BYTECODE_TYPE_CAPTURE:
		ret = side_bytecode_interpret_format_output(estack_ax(stack, top),
				(struct lttng_interpreter_output *) caller_ctx);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret)
		return LTTNG_UST_BYTECODE_INTERPRETER_ERROR;
	else
		return LTTNG_UST_BYTECODE_INTERPRETER_OK;
}

/*
 * The per-event filter dispatch (lttng_ust_interpret_event_filter)
 * is shared with the tracepoint interpreter: it iterates the
 * bytecode runtimes and calls each runtime's interpreter_func, which
 * the side linker points at lttng_bytecode_interpret_side.
 */

#undef START_OP
#undef OP
#undef PO
#undef END_OP
