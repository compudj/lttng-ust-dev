/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Holds LTTng SIDE instrumentation integration.
 */

#define _LGPL_SOURCE
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <iconv.h>
#include <errno.h>
#include <limits.h>

#include <side/trace.h>

#include <lttng/ust-events.h>
#include <lttng/ust-ringbuffer-context.h>
#include <urcu/list.h>
#include <urcu/system.h>
#include <urcu/compiler.h>
#include <lttng/urcu/urcu-ust.h>

#include "common/events.h"
#include "common/macros.h"
#include "common/logging.h"
#include "lttng-tracer-core.h"
#include "side-visit-description.h"
#include "side-visit-arg-vec.h"


/* TODO: optionally print caller address. */
static bool print_caller = false;

#define MAX_NESTING	32

enum tracer_display_base {
	TRACER_DISPLAY_BASE_2,
	TRACER_DISPLAY_BASE_8,
	TRACER_DISPLAY_BASE_10,
	TRACER_DISPLAY_BASE_16,
};

union int_value {
	uint64_t u[NR_SIDE_INTEGER128_SPLIT];
	int64_t s[NR_SIDE_INTEGER128_SPLIT];
};

struct print_ctx {
	int nesting;			/* Keep track of nesting, useful for tabulations. */
	int item_nr[MAX_NESTING];	/* Item number in current nesting level, useful for comma-separated lists. */
};

static struct side_tracer_handle *tracer_handle;

static uint64_t tracer_key;

static struct side_description_visitor_callbacks description_visitor_callbacks;

static
void tracer_convert_string_to_utf8(const void *p, uint8_t unit_size, enum side_type_label_byte_order byte_order,
		size_t *strlen_with_null,
		char **output_str)
{
	size_t ret, inbytesleft = 0, outbytesleft, bufsize, input_size;
	const char *str = p, *fromcode;
	char *inbuf = (char *) p, *outbuf, *buf;
	iconv_t cd;

	switch (unit_size) {
	case 1:
		if (strlen_with_null)
			*strlen_with_null = strlen(str) + 1;
		*output_str = (char *) str;
		return;
	case 2:
	{
		const uint16_t *p16 = p;

		switch (byte_order) {
		case SIDE_TYPE_BYTE_ORDER_LE:
		{
			fromcode = "UTF-16LE";
			break;
		}
		case SIDE_TYPE_BYTE_ORDER_BE:
		{
			fromcode = "UTF-16BE";
			break;
		}
		default:
			fprintf(stderr, "Unknown byte order\n");
			abort();
		}
		for (; *p16; p16++)
			inbytesleft += 2;
		input_size = inbytesleft + 2;
		/*
		 * Worse case is U+FFFF UTF-16 (2 bytes) converting to
		 * { ef, bf, bf } UTF-8 (3 bytes).
		 */
		bufsize = inbytesleft / 2 * 3 + 1;
		break;
	}
	case 4:
	{
		const uint32_t *p32 = p;

		switch (byte_order) {
		case SIDE_TYPE_BYTE_ORDER_LE:
		{
			fromcode = "UTF-32LE";
			break;
		}
		case SIDE_TYPE_BYTE_ORDER_BE:
		{
			fromcode = "UTF-32BE";
			break;
		}
		default:
			fprintf(stderr, "Unknown byte order\n");
			abort();
		}
		for (; *p32; p32++)
			inbytesleft += 4;
		input_size = inbytesleft + 4;
		/*
		 * Each 4-byte UTF-32 character converts to at most a
		 * 4-byte UTF-8 character.
		 */
		bufsize = inbytesleft + 1;
		break;
	}
	default:
		fprintf(stderr, "Unknown string unit size %" PRIu8 "\n", unit_size);
		abort();
	}

	cd = iconv_open("UTF8", fromcode);
	if (cd == (iconv_t) -1) {
		perror("iconv_open");
		abort();
	}
	buf = malloc(bufsize);
	if (!buf) {
		abort();
	}
	outbuf = (char *) buf;
	outbytesleft = bufsize;
	ret = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
	if (ret == (size_t) -1) {
		perror("iconv");
		abort();
	}
	if (inbytesleft) {
		fprintf(stderr, "Buffer too small to convert string input\n");
		abort();
	}
	(*outbuf++) = '\0';
	if (iconv_close(cd) == -1) {
		perror("iconv_close");
		abort();
	}
	if (strlen_with_null)
		*strlen_with_null = input_size;
	*output_str = buf;
}

static
void tracer_print_type_string(const void *p, uint8_t unit_size, enum side_type_label_byte_order byte_order,
		size_t *strlen_with_null)
{
	char *output_str = NULL;

	tracer_convert_string_to_utf8(p, unit_size, byte_order, strlen_with_null, &output_str);
	printf("\"%s\"", output_str);
	if (output_str != p)
		free(output_str);
}

static
void side_check_value_u64(union int_value v)
{
	if (v.u[SIDE_INTEGER128_SPLIT_HIGH]) {
		fprintf(stderr, "Unexpected integer value\n");
		abort();
	}
}

static
void side_check_value_s64(union int_value v)
{
	if (v.s[SIDE_INTEGER128_SPLIT_LOW] & (1ULL << 63)) {
		if (v.s[SIDE_INTEGER128_SPLIT_HIGH] != ~0LL) {
			fprintf(stderr, "Unexpected integer value\n");
			abort();
		}
	} else {
		if (v.s[SIDE_INTEGER128_SPLIT_HIGH]) {
			fprintf(stderr, "Unexpected integer value\n");
			abort();
		}
	}
}

static
int64_t get_attr_integer64_value(const struct side_attr *attr)
{
	int64_t val;

	switch (side_enum_get(attr->value.type)) {
	case SIDE_ATTR_TYPE_U8:
		val = attr->value.u.integer_value.side_u8;
		break;
	case SIDE_ATTR_TYPE_U16:
		val = attr->value.u.integer_value.side_u16;
		break;
	case SIDE_ATTR_TYPE_U32:
		val = attr->value.u.integer_value.side_u32;
		break;
	case SIDE_ATTR_TYPE_U64:
		val = attr->value.u.integer_value.side_u64;
		break;
	case SIDE_ATTR_TYPE_U128:
	{
		union int_value v = {
			.u = {
				[SIDE_INTEGER128_SPLIT_LOW] = attr->value.u.integer_value.side_u128_split[SIDE_INTEGER128_SPLIT_LOW],
				[SIDE_INTEGER128_SPLIT_HIGH] = attr->value.u.integer_value.side_u128_split[SIDE_INTEGER128_SPLIT_HIGH],
			},
		};
		side_check_value_u64(v);
		val = v.u[SIDE_INTEGER128_SPLIT_LOW];
		break;
	}
	case SIDE_ATTR_TYPE_S8:
		val = attr->value.u.integer_value.side_s8;
		break;
	case SIDE_ATTR_TYPE_S16:
		val = attr->value.u.integer_value.side_s16;
		break;
	case SIDE_ATTR_TYPE_S32:
		val = attr->value.u.integer_value.side_s32;
		break;
	case SIDE_ATTR_TYPE_S64:
		val = attr->value.u.integer_value.side_s64;
		break;
	case SIDE_ATTR_TYPE_S128:
	{
		union int_value v = {
			.s = {
				[SIDE_INTEGER128_SPLIT_LOW] = attr->value.u.integer_value.side_s128_split[SIDE_INTEGER128_SPLIT_LOW],
				[SIDE_INTEGER128_SPLIT_HIGH] = attr->value.u.integer_value.side_s128_split[SIDE_INTEGER128_SPLIT_HIGH],
			},
		};
		side_check_value_s64(v);
		val = v.s[SIDE_INTEGER128_SPLIT_LOW];
		break;
	}
	default:
		fprintf(stderr, "Unexpected attribute type\n");
		abort();
	}
	return val;
}

static
enum tracer_display_base get_attr_display_base(const struct side_attr *_attr, uint32_t nr_attr,
				enum tracer_display_base default_base)
{
	uint32_t i;

	for (i = 0; i < nr_attr; i++) {
		const struct side_attr *attr = &_attr[i];
		char *utf8_str = NULL;
		bool cmp;

		tracer_convert_string_to_utf8(side_ptr_get(attr->key.p), attr->key.unit_size,
			side_enum_get(attr->key.byte_order), NULL, &utf8_str);
		cmp = strcmp(utf8_str, "std.integer.base");
		if (utf8_str != side_ptr_get(attr->key.p))
			free(utf8_str);
		if (!cmp) {
			int64_t val = get_attr_integer64_value(attr);

			switch (val) {
			case 2:
				return TRACER_DISPLAY_BASE_2;
			case 8:
				return TRACER_DISPLAY_BASE_8;
			case 10:
				return TRACER_DISPLAY_BASE_10;
			case 16:
				return TRACER_DISPLAY_BASE_16;
			default:
				fprintf(stderr, "Unexpected integer display base: %" PRId64 "\n", val);
				abort();
			}
		}
	}
	return default_base;	/* Default */
}

static
void tracer_print_attr_type(const char *separator, const struct side_attr *attr)
{
	char *utf8_str = NULL;

	tracer_convert_string_to_utf8(side_ptr_get(attr->key.p), attr->key.unit_size,
		side_enum_get(attr->key.byte_order), NULL, &utf8_str);
	printf("{ key%s \"%s\", value%s ", separator, utf8_str, separator);
	if (utf8_str != side_ptr_get(attr->key.p))
		free(utf8_str);
	switch (side_enum_get(attr->value.type)) {
	case SIDE_ATTR_TYPE_BOOL:
		printf("%s", attr->value.u.bool_value ? "true" : "false");
		break;
	case SIDE_ATTR_TYPE_U8:
		printf("%" PRIu8, attr->value.u.integer_value.side_u8);
		break;
	case SIDE_ATTR_TYPE_U16:
		printf("%" PRIu16, attr->value.u.integer_value.side_u16);
		break;
	case SIDE_ATTR_TYPE_U32:
		printf("%" PRIu32, attr->value.u.integer_value.side_u32);
		break;
	case SIDE_ATTR_TYPE_U64:
		printf("%" PRIu64, attr->value.u.integer_value.side_u64);
		break;
	case SIDE_ATTR_TYPE_U128:
		if (attr->value.u.integer_value.side_u128_split[SIDE_INTEGER128_SPLIT_HIGH] == 0) {
			printf("0x%" PRIx64, attr->value.u.integer_value.side_u128_split[SIDE_INTEGER128_SPLIT_LOW]);
		} else {
			printf("0x%" PRIx64 "%016" PRIx64,
				attr->value.u.integer_value.side_u128_split[SIDE_INTEGER128_SPLIT_HIGH],
				attr->value.u.integer_value.side_u128_split[SIDE_INTEGER128_SPLIT_LOW]);
		}
		break;
	case SIDE_ATTR_TYPE_S8:
		printf("%" PRId8, attr->value.u.integer_value.side_s8);
		break;
	case SIDE_ATTR_TYPE_S16:
		printf("%" PRId16, attr->value.u.integer_value.side_s16);
		break;
	case SIDE_ATTR_TYPE_S32:
		printf("%" PRId32, attr->value.u.integer_value.side_s32);
		break;
	case SIDE_ATTR_TYPE_S64:
		printf("%" PRId64, attr->value.u.integer_value.side_s64);
		break;
	case SIDE_ATTR_TYPE_S128:
		if (attr->value.u.integer_value.side_s128_split[SIDE_INTEGER128_SPLIT_HIGH] == 0) {
			printf("0x%" PRIx64, attr->value.u.integer_value.side_s128_split[SIDE_INTEGER128_SPLIT_LOW]);
		} else {
			printf("0x%" PRIx64 "%016" PRIx64,
				attr->value.u.integer_value.side_s128_split[SIDE_INTEGER128_SPLIT_HIGH],
				attr->value.u.integer_value.side_s128_split[SIDE_INTEGER128_SPLIT_LOW]);
		}
		break;
	case SIDE_ATTR_TYPE_FLOAT_BINARY16:
#if __HAVE_FLOAT16
		printf("%g", (double) attr->value.u.float_value.side_float_binary16);
		break;
#else
		fprintf(stderr, "ERROR: Unsupported binary16 float type\n");
		abort();
#endif
	case SIDE_ATTR_TYPE_FLOAT_BINARY32:
#if __HAVE_FLOAT32
		printf("%g", (double) attr->value.u.float_value.side_float_binary32);
		break;
#else
		fprintf(stderr, "ERROR: Unsupported binary32 float type\n");
		abort();
#endif
	case SIDE_ATTR_TYPE_FLOAT_BINARY64:
#if __HAVE_FLOAT64
		printf("%g", (double) attr->value.u.float_value.side_float_binary64);
		break;
#else
		fprintf(stderr, "ERROR: Unsupported binary64 float type\n");
		abort();
#endif
	case SIDE_ATTR_TYPE_FLOAT_BINARY128:
#if __HAVE_FLOAT128
		printf("%Lg", (long double) attr->value.u.float_value.side_float_binary128);
		break;
#else
		fprintf(stderr, "ERROR: Unsupported binary128 float type\n");
		abort();
#endif
	case SIDE_ATTR_TYPE_STRING:
		tracer_print_type_string(side_ptr_get(attr->value.u.string_value.p),
				attr->value.u.string_value.unit_size,
				side_enum_get(attr->value.u.string_value.byte_order), NULL);
		break;
	default:
		fprintf(stderr, "ERROR: <UNKNOWN ATTRIBUTE TYPE>");
		abort();
	}
	printf(" }");
}

static
void print_attributes(const char *prefix_str, const char *separator,
		const struct side_attr *attr, uint32_t nr_attr)
{
	uint32_t i;

	if (!nr_attr)
		return;
	printf("%s%s [", prefix_str, separator);
	for (i = 0; i < nr_attr; i++) {
		printf("%s", i ? ", " : " ");
		tracer_print_attr_type(separator, &attr[i]);
	}
	printf(" ]");
}

static
union int_value tracer_load_integer_value(const struct side_type_integer *type_integer,
		const union side_integer_value *value,
		uint16_t offset_bits, uint16_t *_len_bits)
{
	union int_value v = {};
	uint16_t len_bits;
	bool reverse_bo;

	if (!type_integer->len_bits)
		len_bits = type_integer->integer_size * CHAR_BIT;
	else
		len_bits = type_integer->len_bits;
	if (len_bits + offset_bits > type_integer->integer_size * CHAR_BIT)
		abort();
	reverse_bo = side_enum_get(type_integer->byte_order) != SIDE_TYPE_BYTE_ORDER_HOST;
	switch (type_integer->integer_size) {
	case 1:
		if (type_integer->signedness)
			v.s[SIDE_INTEGER128_SPLIT_LOW] = value->side_s8;
		else
			v.u[SIDE_INTEGER128_SPLIT_LOW] = value->side_u8;
		break;
	case 2:
		if (type_integer->signedness) {
			int16_t side_s16;

			side_s16 = value->side_s16;
			if (reverse_bo)
				side_s16 = side_bswap_16(side_s16);
			v.s[SIDE_INTEGER128_SPLIT_LOW] = side_s16;
		} else {
			uint16_t side_u16;

			side_u16 = value->side_u16;
			if (reverse_bo)
				side_u16 = side_bswap_16(side_u16);
			v.u[SIDE_INTEGER128_SPLIT_LOW] = side_u16;
		}
		break;
	case 4:
		if (type_integer->signedness) {
			int32_t side_s32;

			side_s32 = value->side_s32;
			if (reverse_bo)
				side_s32 = side_bswap_32(side_s32);
			v.s[SIDE_INTEGER128_SPLIT_LOW] = side_s32;
		} else {
			uint32_t side_u32;

			side_u32 = value->side_u32;
			if (reverse_bo)
				side_u32 = side_bswap_32(side_u32);
			v.u[SIDE_INTEGER128_SPLIT_LOW] = side_u32;
		}
		break;
	case 8:
		if (type_integer->signedness) {
			int64_t side_s64;

			side_s64 = value->side_s64;
			if (reverse_bo)
				side_s64 = side_bswap_64(side_s64);
			v.s[SIDE_INTEGER128_SPLIT_LOW] = side_s64;
		} else {
			uint64_t side_u64;

			side_u64 = value->side_u64;
			if (reverse_bo)
				side_u64 = side_bswap_64(side_u64);
			v.u[SIDE_INTEGER128_SPLIT_LOW] = side_u64;
		}
		break;
	case 16:
		if (type_integer->signedness) {
			int64_t side_s64[NR_SIDE_INTEGER128_SPLIT];

			side_s64[SIDE_INTEGER128_SPLIT_LOW] = value->side_s128_split[SIDE_INTEGER128_SPLIT_LOW];
			side_s64[SIDE_INTEGER128_SPLIT_HIGH] = value->side_s128_split[SIDE_INTEGER128_SPLIT_HIGH];
			if (reverse_bo) {
				side_s64[SIDE_INTEGER128_SPLIT_LOW] = side_bswap_64(side_s64[SIDE_INTEGER128_SPLIT_LOW]);
				side_s64[SIDE_INTEGER128_SPLIT_HIGH] = side_bswap_64(side_s64[SIDE_INTEGER128_SPLIT_HIGH]);
				v.s[SIDE_INTEGER128_SPLIT_LOW] = side_s64[SIDE_INTEGER128_SPLIT_HIGH];
				v.s[SIDE_INTEGER128_SPLIT_HIGH] = side_s64[SIDE_INTEGER128_SPLIT_LOW];
			} else {
				v.s[SIDE_INTEGER128_SPLIT_LOW] = side_s64[SIDE_INTEGER128_SPLIT_LOW];
				v.s[SIDE_INTEGER128_SPLIT_HIGH] = side_s64[SIDE_INTEGER128_SPLIT_HIGH];
			}
		} else {
			uint64_t side_u64[NR_SIDE_INTEGER128_SPLIT];

			side_u64[SIDE_INTEGER128_SPLIT_LOW] = value->side_u128_split[SIDE_INTEGER128_SPLIT_LOW];
			side_u64[SIDE_INTEGER128_SPLIT_HIGH] = value->side_u128_split[SIDE_INTEGER128_SPLIT_HIGH];
			if (reverse_bo) {
				side_u64[SIDE_INTEGER128_SPLIT_LOW] = side_bswap_64(side_u64[SIDE_INTEGER128_SPLIT_LOW]);
				side_u64[SIDE_INTEGER128_SPLIT_HIGH] = side_bswap_64(side_u64[SIDE_INTEGER128_SPLIT_HIGH]);
				v.u[SIDE_INTEGER128_SPLIT_LOW] = side_u64[SIDE_INTEGER128_SPLIT_HIGH];
				v.u[SIDE_INTEGER128_SPLIT_HIGH] = side_u64[SIDE_INTEGER128_SPLIT_LOW];
			} else {
				v.u[SIDE_INTEGER128_SPLIT_LOW] = side_u64[SIDE_INTEGER128_SPLIT_LOW];
				v.u[SIDE_INTEGER128_SPLIT_HIGH] = side_u64[SIDE_INTEGER128_SPLIT_HIGH];
			}
		}
		break;
	default:
		abort();
	}
	if (type_integer->integer_size <= 8) {
		v.u[SIDE_INTEGER128_SPLIT_LOW] >>= offset_bits;
		if (len_bits < 64) {
			v.u[SIDE_INTEGER128_SPLIT_LOW] &= (1ULL << len_bits) - 1;
			if (type_integer->signedness) {
				/* Sign-extend. */
				if (v.u[SIDE_INTEGER128_SPLIT_LOW] & (1ULL << (len_bits - 1))) {
					v.u[SIDE_INTEGER128_SPLIT_LOW] |= ~((1ULL << len_bits) - 1);
					v.u[SIDE_INTEGER128_SPLIT_HIGH] = ~0ULL;
				}
			}
		}
	} else {
		//TODO: Implement 128-bit integer with len_bits != 128 or nonzero offset_bits
		if (len_bits < 128 || offset_bits != 0)
			abort();
	}
	if (_len_bits)
		*_len_bits = len_bits;
	return v;
}

static
void print_enum_labels(const struct side_enum_mappings *mappings, union int_value v)
{
	uint32_t print_count = 0;

	side_check_value_s64(v);
	printf(", labels: [ ");
	const struct side_enum_mapping *mapping;
	side_for_each_element_in_array(mapping, &mappings->mappings) {

		if (mapping->range_end < mapping->range_begin) {
			fprintf(stderr, "ERROR: Unexpected enum range: %" PRIu64 "-%" PRIu64 "\n",
				mapping->range_begin, mapping->range_end);
			abort();
		}
		if (v.s[SIDE_INTEGER128_SPLIT_LOW] >= mapping->range_begin && v.s[SIDE_INTEGER128_SPLIT_LOW] <= mapping->range_end) {
			printf("%s", print_count++ ? ", " : "");
			tracer_print_type_string(side_ptr_get(mapping->label.p), mapping->label.unit_size,
				side_enum_get(mapping->label.byte_order), NULL);
		}
	}
	if (!print_count)
		printf("<NO LABEL>");
	printf(" ]");
}

static
uint32_t elem_type_to_stride(const struct side_type *elem_type)
{
	uint32_t stride_bit;

	switch (side_enum_get(elem_type->type)) {
	case SIDE_TYPE_BYTE:
		stride_bit = 8;
		break;

	case SIDE_TYPE_U8:
	case SIDE_TYPE_U16:
	case SIDE_TYPE_U32:
	case SIDE_TYPE_U64:
	case SIDE_TYPE_U128:
	case SIDE_TYPE_S8:
	case SIDE_TYPE_S16:
	case SIDE_TYPE_S32:
	case SIDE_TYPE_S64:
	case SIDE_TYPE_S128:
		return elem_type->u.side_integer.integer_size * CHAR_BIT;
	default:
		fprintf(stderr, "ERROR: Unexpected enum bitmap element type\n");
		abort();
	}
	return stride_bit;
}

static
void print_integer_binary(uint64_t v[NR_SIDE_INTEGER128_SPLIT], int bits)
{
	int bit;

	printf("0b");
	if (bits > 64) {
		bits -= 64;
		v[SIDE_INTEGER128_SPLIT_HIGH] <<= 64 - bits;
		for (bit = 0; bit < bits; bit++) {
			printf("%c", v[SIDE_INTEGER128_SPLIT_HIGH] & (1ULL << 63) ? '1' : '0');
			v[SIDE_INTEGER128_SPLIT_HIGH] <<= 1;
		}
		bits = 64;
	}
	v[SIDE_INTEGER128_SPLIT_LOW] <<= 64 - bits;
	for (bit = 0; bit < bits; bit++) {
		printf("%c", v[SIDE_INTEGER128_SPLIT_LOW] & (1ULL << 63) ? '1' : '0');
		v[SIDE_INTEGER128_SPLIT_LOW] <<= 1;
	}
}

static
void tracer_print_type_header(const char *prefix, const char *separator,
		const struct side_attr *attr, uint32_t nr_attr)
{
	print_attributes("attr", separator, attr, nr_attr);
	printf("%s", nr_attr ? ", " : "");
	printf("%s%s ", prefix, separator);
}

static
void tracer_print_type_bool(const char *separator,
		const struct side_type_bool *type_bool,
		const union side_bool_value *value,
		uint16_t offset_bits)
{
	uint32_t len_bits;
	bool reverse_bo;
	uint64_t v;

	if (!type_bool->len_bits)
		len_bits = type_bool->bool_size * CHAR_BIT;
	else
		len_bits = type_bool->len_bits;
	if (len_bits + offset_bits > type_bool->bool_size * CHAR_BIT)
		abort();
	reverse_bo = side_enum_get(type_bool->byte_order) != SIDE_TYPE_BYTE_ORDER_HOST;
	switch (type_bool->bool_size) {
	case 1:
		v = value->side_bool8;
		break;
	case 2:
	{
		uint16_t side_u16;

		side_u16 = value->side_bool16;
		if (reverse_bo)
			side_u16 = side_bswap_16(side_u16);
		v = side_u16;
		break;
	}
	case 4:
	{
		uint32_t side_u32;

		side_u32 = value->side_bool32;
		if (reverse_bo)
			side_u32 = side_bswap_32(side_u32);
		v = side_u32;
		break;
	}
	case 8:
	{
		uint64_t side_u64;

		side_u64 = value->side_bool64;
		if (reverse_bo)
			side_u64 = side_bswap_64(side_u64);
		v = side_u64;
		break;
	}
	default:
		abort();
	}
	v >>= offset_bits;
	if (len_bits < 64)
		v &= (1ULL << len_bits) - 1;
	tracer_print_type_header("value", separator, side_array_elements(&type_bool->attributes), side_array_length(&type_bool->attributes));
	printf("%s", v ? "true" : "false");
}

/* 2^128 - 1 */
#define U128_BASE_10_ARRAY_LEN	sizeof("340282366920938463463374607431768211455")
/* -2^127 */
#define S128_BASE_10_ARRAY_LEN	sizeof("-170141183460469231731687303715884105728")

/*
 * u128_tostring_base_10 is inspired from https://stackoverflow.com/a/4364365
 */
static
void u128_tostring_base_10(union int_value v, char str[U128_BASE_10_ARRAY_LEN])
{
	int d[39] = {}, i, j, str_i = 0;

	for (i = 63; i > -1; i--) {
		if ((v.u[SIDE_INTEGER128_SPLIT_HIGH] >> i) & 1)
			d[0]++;
		for (j = 0; j < 39; j++)
			d[j] *= 2;
		for (j = 0; j < 38; j++) {
			d[j + 1] += d[j] / 10;
			d[j] %= 10;
		}
	}
	for (i = 63; i > -1; i--) {
		if ((v.u[SIDE_INTEGER128_SPLIT_LOW] >> i) & 1)
			d[0]++;
		if (i > 0) {
			for (j = 0; j < 39; j++)
				d[j] *= 2;
		}
		for (j = 0; j < 38; j++) {
			d[j + 1] += d[j] / 10;
			d[j] %= 10;
		}
	}
	for (i = 38; i > 0; i--)
		if (d[i] > 0)
			break;
	for (; i > -1; i--) {
		str[str_i++] = '0' + d[i];
	}
	str[str_i] = '\0';
}

static
void s128_tostring_base_10(union int_value v, char str[S128_BASE_10_ARRAY_LEN])
{
	uint64_t low, high, tmp;

	if (v.s[SIDE_INTEGER128_SPLIT_HIGH] >= 0) {
		/* Positive. */
		v.u[SIDE_INTEGER128_SPLIT_LOW] = (uint64_t) v.s[SIDE_INTEGER128_SPLIT_LOW];
		v.u[SIDE_INTEGER128_SPLIT_HIGH] = (uint64_t) v.s[SIDE_INTEGER128_SPLIT_HIGH];
		u128_tostring_base_10(v, str);
		return;
	}

	/* Negative. */

	/* Special-case minimum value, which has no positive signed representation. */
	if ((v.s[SIDE_INTEGER128_SPLIT_HIGH] == INT64_MIN) && (v.s[SIDE_INTEGER128_SPLIT_LOW] == 0)) {
		memcpy(str, "-170141183460469231731687303715884105728", S128_BASE_10_ARRAY_LEN);
		return;
	}
	/* Convert from two's complement. */
	high = ~(uint64_t) v.s[SIDE_INTEGER128_SPLIT_HIGH];
	low = ~(uint64_t) v.s[SIDE_INTEGER128_SPLIT_LOW];
	tmp = low + 1;
	if (tmp < low) {
		high++;
		/* Clear overflow to sign bit. */
		high &= ~0x8000000000000000ULL;
	}
	v.u[SIDE_INTEGER128_SPLIT_LOW] = tmp;
	v.u[SIDE_INTEGER128_SPLIT_HIGH] = high;
	str[0] = '-';
	u128_tostring_base_10(v, str + 1);
}

/* 2^128 - 1 */
#define U128_BASE_8_ARRAY_LEN	sizeof("3777777777777777777777777777777777777777777")

static
void u128_tostring_base_8(union int_value v, char str[U128_BASE_8_ARRAY_LEN])
{
	int d[43] = {}, i, j, str_i = 0;

	for (i = 63; i > -1; i--) {
		if ((v.u[SIDE_INTEGER128_SPLIT_HIGH] >> i) & 1)
			d[0]++;
		for (j = 0; j < 43; j++)
			d[j] *= 2;
		for (j = 0; j < 42; j++) {
			d[j + 1] += d[j] / 8;
			d[j] %= 8;
		}
	}
	for (i = 63; i > -1; i--) {
		if ((v.u[SIDE_INTEGER128_SPLIT_LOW] >> i) & 1)
			d[0]++;
		if (i > 0) {
			for (j = 0; j < 43; j++)
				d[j] *= 2;
		}
		for (j = 0; j < 42; j++) {
			d[j + 1] += d[j] / 8;
			d[j] %= 8;
		}
	}
	for (i = 42; i > 0; i--)
		if (d[i] > 0)
			break;
	for (; i > -1; i--) {
		str[str_i++] = '0' + d[i];
	}
	str[str_i] = '\0';
}

static
void tracer_print_type_integer(const char *separator,
		const struct side_type_integer *type_integer,
		const union side_integer_value *value,
		uint16_t offset_bits,
		enum tracer_display_base default_base)
{
	enum tracer_display_base base;
	union int_value v;
	uint16_t len_bits;

	v = tracer_load_integer_value(type_integer, value, offset_bits, &len_bits);
	tracer_print_type_header("value", separator, side_array_elements(&type_integer->attributes), side_array_length(&type_integer->attributes));
	base = get_attr_display_base(side_array_elements(&type_integer->attributes), side_array_length(&type_integer->attributes), default_base);
	switch (base) {
	case TRACER_DISPLAY_BASE_2:
		print_integer_binary(v.u, len_bits);
		break;
	case TRACER_DISPLAY_BASE_8:
		/* Clear sign bits beyond len_bits */
		if (len_bits < 64) {
			v.u[SIDE_INTEGER128_SPLIT_LOW] &= (1ULL << len_bits) - 1;
			v.u[SIDE_INTEGER128_SPLIT_HIGH] = 0;
		} else if (len_bits < 128) {
			v.u[SIDE_INTEGER128_SPLIT_HIGH] &= (1ULL << (len_bits - 64)) - 1;
		}
		if (len_bits <= 64) {
			printf("0o%" PRIo64, v.u[SIDE_INTEGER128_SPLIT_LOW]);
		} else {
			char str[U128_BASE_8_ARRAY_LEN];

			u128_tostring_base_8(v, str);
			printf("0o%s", str);
		}
		break;
	case TRACER_DISPLAY_BASE_10:
		if (len_bits <= 64) {
			if (type_integer->signedness)
				printf("%" PRId64, v.s[SIDE_INTEGER128_SPLIT_LOW]);
			else
				printf("%" PRIu64, v.u[SIDE_INTEGER128_SPLIT_LOW]);
		} else {
			if (type_integer->signedness) {
				char str[S128_BASE_10_ARRAY_LEN];
				s128_tostring_base_10(v, str);
				printf("%s", str);
			} else {
				char str[U128_BASE_10_ARRAY_LEN];
				u128_tostring_base_10(v, str);
				printf("%s", str);
			}
		}
		break;
	case TRACER_DISPLAY_BASE_16:
		/* Clear sign bits beyond len_bits */
		if (len_bits < 64) {
			v.u[SIDE_INTEGER128_SPLIT_LOW] &= (1ULL << len_bits) - 1;
			v.u[SIDE_INTEGER128_SPLIT_HIGH] = 0;
		} else if (len_bits < 128) {
			v.u[SIDE_INTEGER128_SPLIT_HIGH] &= (1ULL << (len_bits - 64)) - 1;
		}
		if (len_bits <= 64 || v.u[SIDE_INTEGER128_SPLIT_HIGH] == 0) {
			printf("0x%" PRIx64, v.u[SIDE_INTEGER128_SPLIT_LOW]);
		} else {
			printf("0x%" PRIx64 "%016" PRIx64,
				v.u[SIDE_INTEGER128_SPLIT_HIGH],
				v.u[SIDE_INTEGER128_SPLIT_LOW]);
		}
		break;
	default:
		abort();
	}
}

static
void tracer_print_type_float(const char *separator,
		const struct side_type_float *type_float,
		const union side_float_value *value)
{
	bool reverse_bo;

	tracer_print_type_header("value", separator, side_array_elements(&type_float->attributes), side_array_length(&type_float->attributes));
	reverse_bo = side_enum_get(type_float->byte_order) != SIDE_TYPE_FLOAT_WORD_ORDER_HOST;
	switch (type_float->float_size) {
	case 2:
	{
#if __HAVE_FLOAT16
		union {
			_Float16 f;
			uint16_t u;
		} float16 = {
			.f = value->side_float_binary16,
		};

		if (reverse_bo)
			float16.u = side_bswap_16(float16.u);
		printf("%g", (double) float16.f);
		break;
#else
		fprintf(stderr, "ERROR: Unsupported binary16 float type\n");
		abort();
#endif
	}
	case 4:
	{
#if __HAVE_FLOAT32
		union {
			_Float32 f;
			uint32_t u;
		} float32 = {
			.f = value->side_float_binary32,
		};

		if (reverse_bo)
			float32.u = side_bswap_32(float32.u);
		printf("%g", (double) float32.f);
		break;
#else
		fprintf(stderr, "ERROR: Unsupported binary32 float type\n");
		abort();
#endif
	}
	case 8:
	{
#if __HAVE_FLOAT64
		union {
			_Float64 f;
			uint64_t u;
		} float64 = {
			.f = value->side_float_binary64,
		};

		if (reverse_bo)
			float64.u = side_bswap_64(float64.u);
		printf("%g", (double) float64.f);
		break;
#else
		fprintf(stderr, "ERROR: Unsupported binary64 float type\n");
		abort();
#endif
	}
	case 16:
	{
#if __HAVE_FLOAT128
		union {
			_Float128 f;
			char arr[16];
		} float128 = {
			.f = value->side_float_binary128,
		};

		if (reverse_bo)
			side_bswap_128p(float128.arr);
		printf("%Lg", (long double) float128.f);
		break;
#else
		fprintf(stderr, "ERROR: Unsupported binary128 float type\n");
		abort();
#endif
	}
	default:
		fprintf(stderr, "ERROR: Unknown float size\n");
		abort();
	}
}

static
void push_nesting(struct print_ctx *ctx)
{
	if (++ctx->nesting >= MAX_NESTING) {
		fprintf(stderr, "ERROR: Nesting too deep.\n");
		abort();
	}
	ctx->item_nr[ctx->nesting] = 0;
}

static
void pop_nesting(struct print_ctx *ctx)
{
	ctx->item_nr[ctx->nesting] = 0;
	if (ctx->nesting-- <= 0) {
		fprintf(stderr, "ERROR: Nesting underflow.\n");
		abort();
	}
}

static
int get_nested_item_nr(struct print_ctx *ctx)
{
	return ctx->item_nr[ctx->nesting];
}

static
void inc_nested_item_nr(struct print_ctx *ctx)
{
	ctx->item_nr[ctx->nesting]++;
}

static
void tracer_before_print_event(const struct side_event_description *desc,
		const struct side_arg_vec *side_arg_vec,
		const struct side_arg_dynamic_struct *var_struct __attribute__((unused)),
		void *caller_addr, void *priv __attribute__((unused)))
{
	uint32_t side_sav_len = side_arg_vec->len;

	if (side_array_length(&desc->fields) != side_sav_len) {
		fprintf(stderr, "ERROR: number of fields mismatch between description and arguments\n");
		abort();
	}

	if (print_caller)
		printf("caller: [%p], ", caller_addr);
	printf("provider: %s, event: %s",
		side_ptr_rel_get(desc->provider_name),
		side_ptr_rel_get(desc->event_name));
	print_attributes(", attr", ":", side_array_rel_elements(&desc->attributes), side_array_length(&desc->attributes));
}

static
void tracer_after_print_event(const struct side_event_description *desc __attribute__((unused)),
		const struct side_arg_vec *side_arg_vec __attribute__((unused)),
		const struct side_arg_dynamic_struct *var_struct __attribute__((unused)),
		void *caller_addr __attribute__((unused)), void *priv __attribute__((unused)))
{
	printf("\n");
}

static
void tracer_before_print_static_fields(const struct side_arg_vec *side_arg_vec, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;
	uint32_t side_sav_len = side_arg_vec->len;

	printf("%s", side_sav_len ? ", fields: {" : "");
	push_nesting(ctx);
}


static
void tracer_after_print_static_fields(const struct side_arg_vec *side_arg_vec, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;
	uint32_t side_sav_len = side_arg_vec->len;

	pop_nesting(ctx);
	if (side_sav_len)
		printf(" }");
}

static
void tracer_before_print_variadic_fields(const struct side_arg_dynamic_struct *var_struct,
		void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;
	uint32_t var_struct_len = var_struct->len;

	print_attributes(", attr ", "::", side_array_elements(&var_struct->attributes), side_array_length(&var_struct->attributes));
	printf("%s", var_struct_len ? ", fields:: {" : "");
	push_nesting(ctx);
}

static
void tracer_after_print_variadic_fields(const struct side_arg_dynamic_struct *var_struct, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;
	uint32_t var_struct_len = var_struct->len;

	pop_nesting(ctx);
	if (var_struct_len)
		printf(" }");
}

static
void tracer_before_print_field(const struct side_event_field *item_desc, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	if (get_nested_item_nr(ctx) != 0)
		printf(",");
	printf(" %s: { ", side_ptr_rel_get(item_desc->field_name));
}

static
void tracer_after_print_field(const struct side_event_field *item_desc __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	printf(" }");
	inc_nested_item_nr(ctx);
}

static
void tracer_before_print_elem(const struct side_type *type_desc __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	if (get_nested_item_nr(ctx) != 0)
		printf(", { ");
	else
		printf(" { ");
}

static
void tracer_after_print_elem(const struct side_type *type_desc __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	printf(" }");
	inc_nested_item_nr(ctx);
}

static
void tracer_print_null(const struct side_type *type_desc,
		const struct side_arg *item __attribute__((unused)),
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("value", ":", side_array_elements(&type_desc->u.side_null.attributes),
				side_array_length(&type_desc->u.side_null.attributes));
	printf("<NULL TYPE>");
}

static
void tracer_print_bool(const struct side_type *type_desc,
		const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_bool(":", &type_desc->u.side_bool, &item->u.side_static.bool_value, 0);
}

static
void tracer_print_integer(const struct side_type *type_desc,
		const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_integer(":", &type_desc->u.side_integer, &item->u.side_static.integer_value, 0, TRACER_DISPLAY_BASE_10);
}

static
void tracer_print_byte(const struct side_type *type_desc __attribute__((unused)),
		const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("value", ":", side_array_elements(&type_desc->u.side_byte.attributes), side_array_length(&type_desc->u.side_byte.attributes));
	printf("0x%" PRIx8, item->u.side_static.byte_value);
}

static
void tracer_print_pointer(const struct side_type *type_desc,
		const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_integer(":", &type_desc->u.side_integer, &item->u.side_static.integer_value, 0, TRACER_DISPLAY_BASE_16);
}

static
void tracer_print_float(const struct side_type *type_desc,
		const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_float(":", &type_desc->u.side_float, &item->u.side_static.float_value);
}

static
void tracer_print_string(const struct side_type *type_desc,
		const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("value", ":", side_array_elements(&type_desc->u.side_string.attributes), side_array_length(&type_desc->u.side_string.attributes));
	tracer_print_type_string(side_ptr_get(item->u.side_static.string_value),
			type_desc->u.side_string.unit_size,
			side_enum_get(type_desc->u.side_string.byte_order), NULL);
}

static
void tracer_before_print_struct(const struct side_type_struct *side_struct,
	const struct side_arg_vec *side_arg_vec __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_struct->attributes), side_array_length(&side_struct->attributes));
	printf("%s", side_array_length(&side_struct->attributes) ? ", " : "");
	printf("fields: {");
	push_nesting(ctx);
}


static
void tracer_after_print_struct(const struct side_type_struct *side_struct __attribute__((unused)),
	const struct side_arg_vec *side_arg_vec __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" }");
}

static
void tracer_before_print_array(const struct side_type_array *side_array,
	const struct side_arg_vec *side_arg_vec __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_array->attributes), side_array_length(&side_array->attributes));
	printf("%s", side_array_length(&side_array->attributes) ? ", " : "");
	printf("elements: [");
	push_nesting(ctx);
}

static
void tracer_after_print_array(const struct side_type_array *side_array __attribute__((unused)),
	const struct side_arg_vec *side_arg_vec __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" ]");
}

static
void do_tracer_before_print_vla(const struct side_type_vla *side_vla,
	const struct side_arg_vec *side_arg_vec __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_vla->attributes), side_array_length(&side_vla->attributes));
	printf("%s", side_array_length(&side_vla->attributes) ? ", " : "");
	printf("elements: [");
	push_nesting(ctx);
}


static
void do_tracer_after_print_vla(const struct side_type_vla *side_vla __attribute__((unused)),
	const struct side_arg_vec *side_arg_vec __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" ]");
}

static
void tracer_before_print_vla(const struct side_type_vla *side_vla,
	const struct side_arg_vec *side_arg_vec, void *priv)
{
	switch (side_enum_get(side_ptr_rel_get(side_vla->length_type)->type)) {
	case SIDE_TYPE_U8:		/* Fall-through */
	case SIDE_TYPE_U16:		/* Fall-through */
	case SIDE_TYPE_U32:		/* Fall-through */
	case SIDE_TYPE_U64:		/* Fall-through */
	case SIDE_TYPE_U128:		/* Fall-through */
	case SIDE_TYPE_S8:		/* Fall-through */
	case SIDE_TYPE_S16:		/* Fall-through */
	case SIDE_TYPE_S32:		/* Fall-through */
	case SIDE_TYPE_S64:		/* Fall-through */
	case SIDE_TYPE_S128:
		break;
	default:
		fprintf(stderr, "ERROR: Unexpected vla length type\n");
		abort();
	}
	do_tracer_before_print_vla(side_vla, side_arg_vec, priv);
}

static
void tracer_after_print_vla(const struct side_type_vla *side_vla,
	const struct side_arg_vec *side_arg_vec, void *priv)
{
	do_tracer_after_print_vla(side_vla, side_arg_vec, priv);
}

static void tracer_print_enum(const struct side_type *type_desc,
	const struct side_arg *item, void *priv)
{
	const struct side_enum_mappings *mappings = side_ptr_get(type_desc->u.side_enum.mappings);
	const struct side_type *elem_type = side_ptr_rel_get(type_desc->u.side_enum.elem_type);
	union int_value v;

	if (side_enum_get(elem_type->type) != side_enum_get(item->type)) {
		fprintf(stderr, "ERROR: Unexpected enum element type\n");
		abort();
	}
	v = tracer_load_integer_value(&elem_type->u.side_integer,
			&item->u.side_static.integer_value, 0, NULL);
	print_attributes("attr", ":", side_array_elements(&mappings->attributes), side_array_length(&mappings->attributes));
	printf("%s", side_array_length(&mappings->attributes) ? ", " : "");
	printf("{ ");
	tracer_print_integer(elem_type, item, priv);
	printf(" }");
	print_enum_labels(mappings, v);
}

static void tracer_print_enum_bitmap(const struct side_type *type_desc,
	const struct side_arg *item, void *priv __attribute__((unused)))
{
	const struct side_enum_bitmap_mappings *side_enum_mappings = side_ptr_get(type_desc->u.side_enum_bitmap.mappings);
	const struct side_type *enum_elem_type = side_ptr_rel_get(type_desc->u.side_enum_bitmap.elem_type), *elem_type;
	uint32_t print_count = 0, stride_bit, nr_items;
	const struct side_arg *array_item;
	const struct side_enum_bitmap_mapping *mapping;

	switch (side_enum_get(enum_elem_type->type)) {
	case SIDE_TYPE_U8:		/* Fall-through */
	case SIDE_TYPE_BYTE:		/* Fall-through */
	case SIDE_TYPE_U16:		/* Fall-through */
	case SIDE_TYPE_U32:		/* Fall-through */
	case SIDE_TYPE_U64:		/* Fall-through */
	case SIDE_TYPE_U128:		/* Fall-through */
	case SIDE_TYPE_S8:		/* Fall-through */
	case SIDE_TYPE_S16:		/* Fall-through */
	case SIDE_TYPE_S32:		/* Fall-through */
	case SIDE_TYPE_S64:		/* Fall-through */
	case SIDE_TYPE_S128:
		elem_type = enum_elem_type;
		array_item = item;
		nr_items = 1;
		break;
	case SIDE_TYPE_ARRAY:
		elem_type = side_ptr_rel_get(side_ptr_rel_get(enum_elem_type->u.side_array)->elem_type);
		array_item = side_ptr_get(side_ptr_get(item->u.side_static.side_array)->sav);
		nr_items = side_ptr_rel_get(enum_elem_type->u.side_array)->length;
		break;
	case SIDE_TYPE_VLA:
		elem_type = side_ptr_rel_get(side_ptr_rel_get(enum_elem_type->u.side_vla)->elem_type);
		array_item = side_ptr_get(side_ptr_get(item->u.side_static.side_vla)->sav);
		nr_items = side_ptr_get(item->u.side_static.side_vla)->len;
		break;
	default:
		fprintf(stderr, "ERROR: Unexpected enum element type\n");
		abort();
	}
	stride_bit = elem_type_to_stride(elem_type);

	print_attributes("attr", ":", side_array_elements(&side_enum_mappings->attributes), side_array_length(&side_enum_mappings->attributes));
	printf("%s", side_array_length(&side_enum_mappings->attributes) ? ", " : "");
	printf("labels: [ ");
	side_for_each_element_in_array(mapping, &side_enum_mappings->mappings) {

		bool match = false;
		uint64_t bit;

		if (mapping->range_end < mapping->range_begin) {
			fprintf(stderr, "ERROR: Unexpected enum bitmap range: %" PRIu64 "-%" PRIu64 "\n",
				mapping->range_begin, mapping->range_end);
			abort();
		}
		for (bit = mapping->range_begin; bit <= mapping->range_end; bit++) {
			if (bit > (nr_items * stride_bit) - 1)
				break;
			if (side_enum_get(elem_type->type) == SIDE_TYPE_BYTE) {
				uint8_t v = array_item[bit / 8].u.side_static.byte_value;
				if (v & (1ULL << (bit % 8))) {
					match = true;
					goto match;
				}
			} else {
				union int_value v = {};

				v = tracer_load_integer_value(&elem_type->u.side_integer,
						&array_item[bit / stride_bit].u.side_static.integer_value,
						0, NULL);
				side_check_value_u64(v);
				if (v.u[SIDE_INTEGER128_SPLIT_LOW] & (1ULL << (bit % stride_bit))) {
					match = true;
					goto match;
				}
			}
		}
match:
		if (match) {
			printf("%s", print_count++ ? ", " : "");
			tracer_print_type_string(side_ptr_get(mapping->label.p), mapping->label.unit_size,
				side_enum_get(mapping->label.byte_order), NULL);
		}
	}
	if (!print_count)
		printf("<NO LABEL>");
	printf(" ]");
}

static
void tracer_print_gather_bool(const struct side_type_gather_bool *type,
	const union side_bool_value *value,
	void *priv __attribute__((unused)))
{
	tracer_print_type_bool(":", &type->type, value, type->offset_bits);
}

static
void tracer_print_gather_byte(const struct side_type_gather_byte *type,
	const uint8_t *_ptr,
	void *priv __attribute__((unused)))
{
	tracer_print_type_header("value", ":", side_array_elements(&type->type.attributes),
				side_array_length(&type->type.attributes));
	printf("0x%" PRIx8, *_ptr);
}

static
void tracer_print_gather_integer(const struct side_type_gather_integer *type,
	const union side_integer_value *value,
	void *priv __attribute__((unused)))
{
	tracer_print_type_integer(":", &type->type, value, type->offset_bits, TRACER_DISPLAY_BASE_10);
}

static
void tracer_print_gather_pointer(const struct side_type_gather_integer *type,
	const union side_integer_value *value,
	void *priv __attribute__((unused)))
{
	tracer_print_type_integer(":", &type->type, value, type->offset_bits, TRACER_DISPLAY_BASE_16);
}

static
void tracer_print_gather_float(const struct side_type_gather_float *type,
	const union side_float_value *value,
	void *priv __attribute__((unused)))
{
	tracer_print_type_float(":", &type->type, value);
}

static
void tracer_print_gather_string(const struct side_type_gather_string *type,
	const void *p, uint8_t unit_size,
	enum side_type_label_byte_order byte_order,
	size_t strlen_with_null __attribute__((unused)),
	void *priv __attribute__((unused)))
{
	//TODO use strlen_with_null input
	tracer_print_type_header("value", ":", side_array_elements(&type->type.attributes),
				side_array_length(&type->type.attributes));
	tracer_print_type_string(p, unit_size, byte_order, NULL);
}

static
void tracer_before_print_gather_struct(const struct side_type_struct *side_struct, void *priv)
{
	tracer_before_print_struct(side_struct, NULL, priv);
}

static
void tracer_after_print_gather_struct(const struct side_type_struct *side_struct, void *priv)
{
	tracer_after_print_struct(side_struct, NULL, priv);
}

static
void tracer_before_print_gather_array(const struct side_type_array *side_array, void *priv)
{
	tracer_before_print_array(side_array, NULL, priv);
}

static
void tracer_after_print_gather_array(const struct side_type_array *side_array, void *priv)
{
	tracer_after_print_array(side_array, NULL, priv);
}

static
void tracer_before_print_gather_vla(const struct side_type_vla *side_vla,
	uint32_t length __attribute__((unused)), void *priv)
{
	switch (side_enum_get(side_ptr_rel_get(side_vla->length_type)->type)) {
	case SIDE_TYPE_GATHER_INTEGER:
		break;
	default:
		fprintf(stderr, "ERROR: Unexpected vla length type\n");
		abort();
	}
	do_tracer_before_print_vla(side_vla, NULL, priv);
}


static
void tracer_after_print_gather_vla(const struct side_type_vla *side_vla,
	uint32_t length __attribute__((unused)), void *priv)
{
	do_tracer_after_print_vla(side_vla, NULL, priv);
}

static
void tracer_print_gather_enum(const struct side_type_gather_enum *type,
	const union side_integer_value *value,
	void *priv __attribute__((unused)))
{
	const struct side_enum_mappings *mappings = side_ptr_get(type->mappings);
	const struct side_type *enum_elem_type = side_ptr_rel_get(type->elem_type);
	const struct side_type_gather_integer *side_integer = &enum_elem_type->u.side_gather.u.side_integer;
	union int_value v;

	v = tracer_load_integer_value(&side_integer->type, value, 0, NULL);
	print_attributes("attr", ":", side_array_elements(&mappings->attributes), side_array_length(&mappings->attributes));
	printf("%s", side_array_length(&mappings->mappings) ? ", " : "");
	printf("{ ");
	tracer_print_type_integer(":", &side_integer->type, value, 0, TRACER_DISPLAY_BASE_10);
	printf(" }");
	print_enum_labels(mappings, v);
}

static
void tracer_before_print_dynamic_field(const struct side_arg_dynamic_field *field, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	if (get_nested_item_nr(ctx) != 0)
		printf(",");
	printf(" %s:: { ", side_ptr_get(field->field_name));
}

static
void tracer_after_print_dynamic_field(const struct side_arg_dynamic_field *field __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	printf(" }");
	inc_nested_item_nr(ctx);
}

static
void tracer_before_print_dynamic_elem(const struct side_arg *dynamic_item __attribute__((unused)), void *priv)
{
	tracer_before_print_elem(NULL, priv);
}

static
void tracer_after_print_dynamic_elem(const struct side_arg *dynamic_item __attribute__((unused)), void *priv)
{
	tracer_after_print_elem(NULL, priv);
}

static
void tracer_print_dynamic_null(const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("value", "::", side_array_elements(&item->u.side_dynamic.side_null.attributes),
				side_array_length(&item->u.side_dynamic.side_null.attributes));
	printf("<NULL TYPE>");
}

static
void tracer_print_dynamic_bool(const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_bool("::", &item->u.side_dynamic.side_bool.type, &item->u.side_dynamic.side_bool.value, 0);
}

static
void tracer_print_dynamic_integer(const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_integer("::", &item->u.side_dynamic.side_integer.type, &item->u.side_dynamic.side_integer.value, 0,
			TRACER_DISPLAY_BASE_10);
}

static
void tracer_print_dynamic_byte(const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("value", "::", side_array_elements(&item->u.side_dynamic.side_byte.type.attributes), side_array_length(&item->u.side_dynamic.side_byte.type.attributes));
	printf("0x%" PRIx8, item->u.side_dynamic.side_byte.value);
}

static
void tracer_print_dynamic_pointer(const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_integer("::", &item->u.side_dynamic.side_integer.type, &item->u.side_dynamic.side_integer.value, 0,
			TRACER_DISPLAY_BASE_16);
}

static
void tracer_print_dynamic_float(const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_float("::", &item->u.side_dynamic.side_float.type,
			&item->u.side_dynamic.side_float.value);
}

static
void tracer_print_dynamic_string(const struct side_arg *item,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("value", "::", side_array_elements(&item->u.side_dynamic.side_string.type.attributes), side_array_length(&item->u.side_dynamic.side_string.type.attributes));
	tracer_print_type_string((const char *)(uintptr_t) item->u.side_dynamic.side_string.value,
			item->u.side_dynamic.side_string.type.unit_size,
			side_enum_get(item->u.side_dynamic.side_string.type.byte_order), NULL);
}

static
void tracer_before_print_dynamic_struct(const struct side_arg_dynamic_struct *dynamic_struct,
	void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", "::", side_array_elements(&dynamic_struct->attributes), side_array_length(&dynamic_struct->attributes));
	printf("%s", side_array_length(&dynamic_struct->attributes) ? ", " : "");
	printf("fields:: {");
	push_nesting(ctx);
}

static
void tracer_after_print_dynamic_struct(const struct side_arg_dynamic_struct *dynamic_struct __attribute__((unused)),
	void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" }");
}

static
void tracer_before_print_dynamic_vla(const struct side_arg_dynamic_vla *dynamic_vla, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", "::", side_array_elements(&dynamic_vla->attributes), side_array_length(&dynamic_vla->attributes));
	printf("%s", side_array_length(&dynamic_vla->attributes)? ", " : "");
	printf("elements:: [");
	push_nesting(ctx);
}

static
void tracer_after_print_dynamic_vla(const struct side_arg_dynamic_vla *dynamic_vla __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" ]");
}

static struct side_type_visitor type_visitor = {
	.before_event_func = tracer_before_print_event,
	.after_event_func = tracer_after_print_event,
	.before_static_fields_func = tracer_before_print_static_fields,
	.after_static_fields_func = tracer_after_print_static_fields,
	.before_variadic_fields_func = tracer_before_print_variadic_fields,
	.after_variadic_fields_func = tracer_after_print_variadic_fields,

	/* Stack-copy basic types. */
	.before_field_func = tracer_before_print_field,
	.after_field_func = tracer_after_print_field,
	.before_elem_func = tracer_before_print_elem,
	.after_elem_func = tracer_after_print_elem,
	.null_type_func = tracer_print_null,
	.bool_type_func = tracer_print_bool,
	.integer_type_func = tracer_print_integer,
	.byte_type_func = tracer_print_byte,
	.pointer_type_func = tracer_print_pointer,
	.float_type_func = tracer_print_float,
	.string_type_func = tracer_print_string,

	/* Stack-copy compound types. */
	.before_struct_type_func = tracer_before_print_struct,
	.after_struct_type_func = tracer_after_print_struct,
	.before_array_type_func = tracer_before_print_array,
	.after_array_type_func = tracer_after_print_array,
	.before_vla_type_func = tracer_before_print_vla,
	.after_vla_type_func = tracer_after_print_vla,

	/* Stack-copy enumeration types. */
	.enum_type_func = tracer_print_enum,
	.enum_bitmap_type_func = tracer_print_enum_bitmap,

	/* Gather basic types. */
	.gather_bool_type_func = tracer_print_gather_bool,
	.gather_byte_type_func = tracer_print_gather_byte,
	.gather_integer_type_func = tracer_print_gather_integer,
	.gather_pointer_type_func = tracer_print_gather_pointer,
	.gather_float_type_func = tracer_print_gather_float,
	.gather_string_type_func = tracer_print_gather_string,

	/* Gather compound types. */
	.before_gather_struct_type_func = tracer_before_print_gather_struct,
	.after_gather_struct_type_func = tracer_after_print_gather_struct,
	.before_gather_array_type_func = tracer_before_print_gather_array,
	.after_gather_array_type_func = tracer_after_print_gather_array,
	.before_gather_vla_type_func = tracer_before_print_gather_vla,
	.after_gather_vla_type_func = tracer_after_print_gather_vla,

	/* Gather enumeration types. */
	.gather_enum_type_func = tracer_print_gather_enum,

	/* Dynamic basic types. */
	.before_dynamic_field_func = tracer_before_print_dynamic_field,
	.after_dynamic_field_func = tracer_after_print_dynamic_field,
	.before_dynamic_elem_func = tracer_before_print_dynamic_elem,
	.after_dynamic_elem_func = tracer_after_print_dynamic_elem,

	.dynamic_null_func = tracer_print_dynamic_null,
	.dynamic_bool_func = tracer_print_dynamic_bool,
	.dynamic_integer_func = tracer_print_dynamic_integer,
	.dynamic_byte_func = tracer_print_dynamic_byte,
	.dynamic_pointer_func = tracer_print_dynamic_pointer,
	.dynamic_float_func = tracer_print_dynamic_float,
	.dynamic_string_func = tracer_print_dynamic_string,

	/* Dynamic compound types. */
	.before_dynamic_struct_func = tracer_before_print_dynamic_struct,
	.after_dynamic_struct_func = tracer_after_print_dynamic_struct,
	.before_dynamic_vla_func = tracer_before_print_dynamic_vla,
	.after_dynamic_vla_func = tracer_after_print_dynamic_vla,
};

static
__attribute__((unused))
void tracer_print_call(const struct side_event_description *desc,
		const struct side_arg_vec *side_arg_vec,
		void *priv __attribute__((unused)),
		void *caller_addr)
{
	struct print_ctx ctx = {};

	type_visitor_event(&type_visitor, desc, side_arg_vec, NULL, caller_addr, &ctx);
}

static
__attribute__((unused))
void tracer_print_call_variadic(const struct side_event_description *desc,
		const struct side_arg_vec *side_arg_vec,
		const struct side_arg_dynamic_struct *var_struct,
		void *priv __attribute__((unused)),
		void *caller_addr)
{
	struct print_ctx ctx = {};

	type_visitor_event(&type_visitor, desc, side_arg_vec, var_struct, caller_addr, &ctx);
}

static
void before_print_description_event(const struct side_event_description *desc, void *priv __attribute__((unused)))
{
	printf("event description: provider: %s, event: %s", side_ptr_rel_get(desc->provider_name), side_ptr_rel_get(desc->event_name));
	print_attributes(", attr", ":", side_array_rel_elements(&desc->attributes), side_array_length(&desc->attributes));
}

static
void after_print_description_event(const struct side_event_description *desc, void *priv __attribute__((unused)))
{
	if (desc->flags & SIDE_EVENT_FLAG_VARIADIC)
		printf(", <variadic fields>");
	printf("\n");
}

static
void before_print_description_static_fields(const struct side_event_description *desc, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;
	uint32_t len = side_array_length(&desc->fields);

	printf("%s", len ? ", fields: {" : "");
	push_nesting(ctx);
}

static
void after_print_description_static_fields(const struct side_event_description *desc, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;
	uint32_t len = side_array_length(&desc->fields);

	pop_nesting(ctx);
	if (len)
		printf(" }");
}

static
void before_print_description_field(const struct side_event_field *item_desc, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	if (get_nested_item_nr(ctx) != 0)
		printf(",");
	printf(" %s: { ", side_ptr_rel_get(item_desc->field_name));
}

static
void after_print_description_field(const struct side_event_field *item_desc __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	printf(" }");
	inc_nested_item_nr(ctx);
}

static
void before_print_description_elem(const struct side_type *type_desc __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	if (get_nested_item_nr(ctx) != 0)
		printf(", { ");
	else
		printf(" { ");
}

static
void after_print_description_elem(const struct side_type *type_desc __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	printf(" }");
	inc_nested_item_nr(ctx);
}

static
void before_print_description_option(const struct side_variant_option *option_desc, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	if (get_nested_item_nr(ctx) != 0)
		printf(",");
	if (option_desc->range_begin == option_desc->range_end)
		printf(" [ %" PRIu64 " ]: { ",
			option_desc->range_begin);
	else
		printf(" [ %" PRIu64 " - %" PRIu64 " ]: { ",
			option_desc->range_begin,
			option_desc->range_end);
}

static
void after_print_description_option(const struct side_variant_option *option_desc __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	printf(" }");
	inc_nested_item_nr(ctx);
}

static
void print_description_null(const struct side_type_null *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":",
				side_array_elements(&type->attributes),
				side_array_length(&type->attributes));
	printf("null");
}

static
void print_description_bool(const struct side_type_bool *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":",
				side_array_elements(&type->attributes),
				side_array_length(&type->attributes));
	printf("bool { size: %" PRIu16, type->bool_size);
	if (type->len_bits)
		printf(", len_bits: %" PRIu16, type->len_bits);
	printf(" }");
}

static
void print_description_integer(const struct side_type_integer *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":",
				side_array_elements(&type->attributes),
				side_array_length(&type->attributes));
	printf("integer { size: %" PRIu16 ", signedness: %s, byte_order: \"%s\"",
		type->integer_size,
		type->signedness ? "true" : "false",
		side_enum_get(type->byte_order) == SIDE_TYPE_BYTE_ORDER_LE ? "le" : "be");
	if (type->len_bits)
		printf(", len_bits: %" PRIu16, type->len_bits);
	printf(" }");
}

static
void print_description_byte(const struct side_type_byte *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":",
				side_array_elements(&type->attributes),
				side_array_length(&type->attributes));
	printf("byte");
}

static
void print_description_pointer(const struct side_type_integer *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":",
				side_array_elements(&type->attributes),
				side_array_length(&type->attributes));
	printf("pointer { size: %" PRIu16 ", signedness: %s, byte_order: \"%s\"",
		type->integer_size,
		type->signedness ? "true" : "false",
		side_enum_get(type->byte_order) == SIDE_TYPE_BYTE_ORDER_LE ? "le" : "be");
	if (type->len_bits)
		printf(", len_bits: %" PRIu16, type->len_bits);
	printf(" }");
}

static
void print_description_float(const struct side_type_float *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":",
				side_array_elements(&type->attributes),
				side_array_length(&type->attributes));
	printf("float { size: %" PRIu16 ", byte_order: \"%s\"",
		type->float_size,
		side_enum_get(type->byte_order) == SIDE_TYPE_BYTE_ORDER_LE ? "le" : "be");
	printf(" }");
}

static
void print_description_string(const struct side_type_string *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":",
				side_array_elements(&type->attributes),
				side_array_length(&type->attributes));
	printf("string { unit_size: %" PRIu8,
		type->unit_size);
	if (type->unit_size > 1)
		printf(", byte_order: \"%s\"",
			side_enum_get(type->byte_order) == SIDE_TYPE_BYTE_ORDER_LE ? "le" : "be");
	printf(" }");
}

static
void before_print_description_struct(const struct side_type_struct *side_struct, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_struct->attributes), side_array_length(&side_struct->attributes));
	printf("%s", side_array_length(&side_struct->attributes)? ", " : "");
	printf("type: struct { fields: {");
	push_nesting(ctx);
}


static
void after_print_description_struct(const struct side_type_struct *side_struct __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" } }");
}

static
void before_print_description_variant(const struct side_type_variant *side_variant, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_variant->attributes), side_array_length(&side_variant->attributes));
	printf("%s", side_array_length(&side_variant->attributes)? ", " : "");
	printf("type: variant { options: {");
	push_nesting(ctx);
}

static
void after_print_description_variant(const struct side_type_variant *side_variant __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" } }");
}

static
void before_print_description_optional(const struct side_type_optional *optional __attribute__((unused)),
				void *priv __attribute__((unused)))
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	printf("type: optional {");
	push_nesting(ctx);
}

static
void after_print_description_optional(const struct side_type_optional *optional __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" } }");
}

static
void before_print_description_array(const struct side_type_array *side_array, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_array->attributes), side_array_length(&side_array->attributes));
	printf("%s", side_array_length(&side_array->attributes)? ", " : "");
	printf("type: array { length: %" PRIu32 ", element:", side_array->length);
	push_nesting(ctx);
}


static
void after_print_description_array(const struct side_type_array *side_array __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" }");
}

static
void before_print_description_vla(const struct side_type_vla *side_vla, void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_vla->attributes), side_array_length(&side_vla->attributes));
	printf("%s", side_array_length(&side_vla->attributes)? ", " : "");
	printf("type: vla { length:");
	push_nesting(ctx);
}

static
void after_length_print_description_vla(const struct side_type_vla *side_vla __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(", element:");
	push_nesting(ctx);
}

static
void after_element_print_description_vla(const struct side_type_vla *side_vla __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" }");
}

static
void do_before_print_description_enum(const char *type_name, const struct side_enum_mappings *mappings, void *priv __attribute__((unused)))
{
	uint32_t print_count = 0;
	const struct side_enum_mapping *mapping;

	tracer_print_type_header("type", ":", side_array_elements(&mappings->attributes), side_array_length(&mappings->attributes));
	printf("%s { labels: { ", type_name);
	side_for_each_element_in_array (mapping, &mappings->mappings) {

		if (mapping->range_end < mapping->range_begin) {
			fprintf(stderr, "ERROR: Unexpected enum range: %" PRIu64 "-%" PRIu64 "\n",
				mapping->range_begin, mapping->range_end);
			abort();
		}
		printf("%s", print_count++ ? ", " : "");
		if (mapping->range_begin == mapping->range_end)
			printf("[ %" PRIu64 " ]: ", mapping->range_begin);
		else
			printf("[ %" PRIu64 " - %" PRIu64 " ]: ",
				mapping->range_begin, mapping->range_end);
		tracer_print_type_string(side_ptr_get(mapping->label.p), mapping->label.unit_size,
			side_enum_get(mapping->label.byte_order), NULL);
	}
	if (!print_count)
		printf("<NO LABEL>");

	printf(" }, element: { ");
}


static
void do_after_print_description_enum(const char *type_name __attribute__((unused)), const struct side_enum_mappings *mappings __attribute__((unused)), void *priv __attribute__((unused)))
{
	printf(" }");
}

static
void before_print_description_enum(const struct side_type_enum *type, void *priv)
{
	const struct side_enum_mappings *mappings = side_ptr_get(type->mappings);
	const struct side_type *elem_type = side_ptr_rel_get(type->elem_type);

	switch (side_enum_get(elem_type->type)) {
	case SIDE_TYPE_U8:
	case SIDE_TYPE_U16:
	case SIDE_TYPE_U32:
	case SIDE_TYPE_U64:
	case SIDE_TYPE_U128:
	case SIDE_TYPE_S8:
	case SIDE_TYPE_S16:
	case SIDE_TYPE_S32:
	case SIDE_TYPE_S64:
	case SIDE_TYPE_S128:
		break;
	default:
		fprintf(stderr, "Unsupported enum element type.\n");
		abort();
	}
	do_before_print_description_enum("enum", mappings, priv);
}

static
void after_print_description_enum(const struct side_type_enum *type, void *priv)
{
	const struct side_enum_mappings *mappings = side_ptr_get(type->mappings);

	do_after_print_description_enum("enum", mappings, priv);
}

static
void before_print_description_enum_bitmap(const struct side_type_enum_bitmap *type, void *priv __attribute__((unused)))
{
	const struct side_type *elem_type = side_ptr_rel_get(type->elem_type);
	const struct side_enum_bitmap_mappings *mappings = side_ptr_get(type->mappings);
	uint32_t print_count = 0;

	switch (side_enum_get(elem_type->type)) {
	case SIDE_TYPE_BYTE:
	case SIDE_TYPE_U8:
	case SIDE_TYPE_U16:
	case SIDE_TYPE_U32:
	case SIDE_TYPE_U64:
	case SIDE_TYPE_U128:
	case SIDE_TYPE_ARRAY:
	case SIDE_TYPE_VLA:
		break;
	default:
		fprintf(stderr, "Unsupported enum element type.\n");
		abort();
	}
	tracer_print_type_header("type", ":", side_array_elements(&mappings->attributes), side_array_length(&mappings->attributes));
	printf("enum_bitmap { labels: { ");
	const struct side_enum_bitmap_mapping *mapping;
	side_for_each_element_in_array(mapping, &mappings->mappings) {

		if (mapping->range_end < mapping->range_begin) {
			fprintf(stderr, "ERROR: Unexpected enum range: %" PRIu64 "-%" PRIu64 "\n",
				mapping->range_begin, mapping->range_end);
			abort();
		}
		printf("%s", print_count++ ? ", " : "");
		if (mapping->range_begin == mapping->range_end)
			printf("[ %" PRIu64 " ]: ", mapping->range_begin);
		else
			printf("[ %" PRIu64 " - %" PRIu64 " ]: ",
				mapping->range_begin, mapping->range_end);
		tracer_print_type_string(side_ptr_get(mapping->label.p), mapping->label.unit_size,
			side_enum_get(mapping->label.byte_order), NULL);
	}
	if (!print_count)
		printf("<NO LABEL>");

	printf(" }, element: { ");
}

static
void after_print_description_enum_bitmap(const struct side_type_enum_bitmap *type __attribute__((unused)), void *priv __attribute__((unused)))
{
	printf(" }");
}

static
void print_description_gather_bool(const struct side_type_gather_bool *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":", side_array_elements(&type->type.attributes), side_array_length(&type->type.attributes));
	printf("gather_bool { size: %" PRIu16, type->type.bool_size);
	if (type->type.len_bits)
		printf(", len_bits: %" PRIu16, type->type.len_bits);
	printf(", offset: %" PRIu64 ", offset_bits: %" PRIu16 ", access_mode: %s",
		type->offset, type->offset_bits,
		side_enum_get(type->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
	printf(" }");
}

static
void print_description_gather_byte(const struct side_type_gather_byte *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":", side_array_elements(&type->type.attributes), side_array_length(&type->type.attributes));
	printf("gather_byte { offset: %" PRIu64 ", access_mode: %s }",
		type->offset,
		side_enum_get(type->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
}

static
void print_description_gather_integer(const struct side_type_gather_integer *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":", side_array_elements(&type->type.attributes), side_array_length(&type->type.attributes));
	printf("gather_integer { size: %" PRIu16 ", signedness: %s, byte_order: \"%s\"",
		type->type.integer_size,
		type->type.signedness ? "true" : "false",
		side_enum_get(type->type.byte_order) == SIDE_TYPE_BYTE_ORDER_LE ? "le" : "be");
	if (type->type.len_bits)
		printf(", len_bits: %" PRIu16, type->type.len_bits);
	printf(", offset: %" PRIu64 ", offset_bits: %" PRIu16 ", access_mode: %s",
		type->offset, type->offset_bits,
		side_enum_get(type->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
	printf(" }");
}

static
void print_description_gather_pointer(const struct side_type_gather_integer *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":", side_array_elements(&type->type.attributes), side_array_length(&type->type.attributes));
	printf("gather_pointer { size: %" PRIu16 ", signedness: %s, byte_order: \"%s\"",
		type->type.integer_size,
		type->type.signedness ? "true" : "false",
		side_enum_get(type->type.byte_order) == SIDE_TYPE_BYTE_ORDER_LE ? "le" : "be");
	if (type->type.len_bits)
		printf(", len_bits: %" PRIu16, type->type.len_bits);
	printf(", offset: %" PRIu64 ", offset_bits: %" PRIu16 ", access_mode: %s",
		type->offset, type->offset_bits,
		side_enum_get(type->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
	printf(" }");
}

static
void print_description_gather_float(const struct side_type_gather_float *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":", side_array_elements(&type->type.attributes), side_array_length(&type->type.attributes));
	printf("gather_float { size: %" PRIu16 ", byte_order: \"%s\"",
		type->type.float_size,
		side_enum_get(type->type.byte_order) == SIDE_TYPE_BYTE_ORDER_LE ? "le" : "be");
	printf(", offset: %" PRIu64 ", access_mode: %s",
		type->offset,
		side_enum_get(type->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
	printf(" }");
}

static
void print_description_gather_string(const struct side_type_gather_string *type,
		void *priv __attribute__((unused)))
{
	tracer_print_type_header("type", ":", side_array_elements(&type->type.attributes), side_array_length(&type->type.attributes));
	printf("gather_string { unit_size: %" PRIu8,
		type->type.unit_size);
	if (type->type.unit_size > 1)
		printf(", byte_order: \"%s\"",
			side_enum_get(type->type.byte_order) == SIDE_TYPE_BYTE_ORDER_LE ? "le" : "be");
	printf(", offset: %" PRIu64 ", access_mode: %s",
		type->offset,
		side_enum_get(type->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
	printf(" }");
}

static
void before_print_description_gather_struct(const struct side_type_gather_struct *side_gather_struct, void *priv)
{
	const struct side_type_struct *side_struct = side_ptr_rel_get(side_gather_struct->type);
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_struct->attributes), side_array_length(&side_struct->attributes));
	printf("%s", side_array_length(&side_struct->attributes)? ", " : "");
	printf("type: gather_struct { size: %" PRIu32 ", offset: %" PRIu64 ", access_mode: %s, fields: {",
		side_gather_struct->size, side_gather_struct->offset,
		side_enum_get(side_gather_struct->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
	push_nesting(ctx);
}

static
void after_print_description_gather_struct(const struct side_type_gather_struct *side_gather_struct __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" } }");
}

static
void before_print_description_gather_array(const struct side_type_gather_array *side_gather_array, void *priv)
{
	const struct side_type_array *side_array = &side_gather_array->type;
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_array->attributes), side_array_length(&side_array->attributes));
	printf("%s", side_array_length(&side_array->attributes)? ", " : "");
	printf("type: gather_array { offset: %" PRIu64 ", access_mode: %s, element:",
		side_gather_array->offset,
		side_enum_get(side_gather_array->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
	push_nesting(ctx);
}

static
void after_print_description_gather_array(const struct side_type_gather_array *side_gather_array __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" }");
}

static
void before_print_description_gather_vla(const struct side_type_gather_vla *side_gather_vla, void *priv)
{
	const struct side_type_vla *side_vla = &side_gather_vla->type;
	struct print_ctx *ctx = (struct print_ctx *) priv;

	print_attributes("attr", ":", side_array_elements(&side_vla->attributes), side_array_length(&side_vla->attributes));
	printf("%s", side_array_length(&side_vla->attributes)? ", " : "");
	printf("type: gather_vla { offset: %" PRIu64 ", access_mode: %s, length:",
		side_gather_vla->offset,
		side_enum_get(side_gather_vla->access_mode) == SIDE_TYPE_GATHER_ACCESS_DIRECT ? "\"direct\"" : "\"pointer\"");
	push_nesting(ctx);
}

static
void after_length_print_description_gather_vla(const struct side_type_gather_vla *side_gather_vla __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(", element:");
	push_nesting(ctx);
}

static
void after_element_print_description_gather_vla(const struct side_type_gather_vla *side_gather_vla __attribute__((unused)), void *priv)
{
	struct print_ctx *ctx = (struct print_ctx *) priv;

	pop_nesting(ctx);
	printf(" }");
}

static
void before_print_description_gather_enum(const struct side_type_gather_enum *type, void *priv)
{
	const struct side_enum_mappings *mappings = side_ptr_get(type->mappings);
	const struct side_type *elem_type = side_ptr_rel_get(type->elem_type);

	if (side_enum_get(elem_type->type) != SIDE_TYPE_GATHER_INTEGER) {
		fprintf(stderr, "Unsupported enum element type.\n");
		abort();
	}
	do_before_print_description_enum("gather_enum", mappings, priv);
}

static
void after_print_description_gather_enum(const struct side_type_gather_enum *type, void *priv)
{
	const struct side_enum_mappings *mappings = side_ptr_get(type->mappings);

	do_after_print_description_enum("gather_enum", mappings, priv);
}

static
void print_description_dynamic(const struct side_type *type_desc __attribute__((unused)), void *priv __attribute__((unused)))
{
	printf("type: dynamic");
}

static
struct side_description_visitor_callbacks description_visitor_callbacks = {
	.before_event_func = before_print_description_event,
	.after_event_func = after_print_description_event,
	.before_static_fields_func = before_print_description_static_fields,
	.after_static_fields_func = after_print_description_static_fields,

	/* Stack-copy basic types. */
	.before_field_func = before_print_description_field,
	.after_field_func = after_print_description_field,
	.before_elem_func = before_print_description_elem,
	.after_elem_func = after_print_description_elem,
	.before_option_func = before_print_description_option,
	.after_option_func = after_print_description_option,
	.null_type_func = print_description_null,
	.bool_type_func = print_description_bool,
	.integer_type_func = print_description_integer,
	.byte_type_func = print_description_byte,
	.pointer_type_func = print_description_pointer,
	.float_type_func = print_description_float,
	.string_type_func = print_description_string,

	/* Stack-copy compound types. */
	.before_struct_type_func = before_print_description_struct,
	.after_struct_type_func = after_print_description_struct,
	.before_variant_type_func = before_print_description_variant,
	.after_variant_type_func = after_print_description_variant,
	.before_array_type_func = before_print_description_array,
	.after_array_type_func = after_print_description_array,
	.before_vla_type_func = before_print_description_vla,
	.after_length_vla_type_func = after_length_print_description_vla,
	.after_element_vla_type_func = after_element_print_description_vla,
	.before_optional_type_func = before_print_description_optional,
	.after_optional_type_func = after_print_description_optional,

	/* Stack-copy enumeration types. */
	.before_enum_type_func = before_print_description_enum,
	.after_enum_type_func = after_print_description_enum,
	.before_enum_bitmap_type_func = before_print_description_enum_bitmap,
	.after_enum_bitmap_type_func = after_print_description_enum_bitmap,

	/* Gather basic types. */
	.gather_bool_type_func = print_description_gather_bool,
	.gather_byte_type_func = print_description_gather_byte,
	.gather_integer_type_func = print_description_gather_integer,
	.gather_pointer_type_func = print_description_gather_pointer,
	.gather_float_type_func = print_description_gather_float,
	.gather_string_type_func = print_description_gather_string,

	/* Gather compound types. */
	.before_gather_struct_type_func = before_print_description_gather_struct,
	.after_gather_struct_type_func = after_print_description_gather_struct,
	.before_gather_array_type_func = before_print_description_gather_array,
	.after_gather_array_type_func = after_print_description_gather_array,
	.before_gather_vla_type_func = before_print_description_gather_vla,
	.after_length_gather_vla_type_func = after_length_print_description_gather_vla,
	.after_element_gather_vla_type_func = after_element_print_description_gather_vla,

	/* Gather enumeration types. */
	.before_gather_enum_type_func = before_print_description_gather_enum,
	.after_gather_enum_type_func = after_print_description_gather_enum,

	/* Dynamic types. */
	.dynamic_type_func = print_description_dynamic,
};

static
__attribute__((unused))
void print_event_description(const struct side_event_description *desc)
{
	struct print_ctx ctx = {};
	struct side_description_visitor visitor = {
		.callbacks = &description_visitor_callbacks,
		.priv = &ctx,
	};

	visit_event_description(&visitor, desc);
}

/* ==================== LTTng tracing integration ==================== */

/*
 * Side event descriptions are translated into dynamically allocated
 * LTTng probe/event descriptors registered with the probe provider
 * registry, from which the existing enabler machinery creates
 * events. The register_event()/unregister_event() paths connect
 * LTTng events to side by registering the tracer_call() callback on
 * the side event (see lttng_ust_side_register_event), which flips
 * the side instrumentation "enabled" state: disabled side events
 * keep their lock-free disabled fast path.
 *
 * Lock ordering (see the side.c lock ordering comment):
 * side_notification_lock (held across tracer_event_notification)
 *   -> ust_mutex (taken by lttng_ust_probe_register/unregister)
 *     -> side_event_lock (leaf, taken by
 *        side_tracer_callback_register/unregister from
 *        register_event/unregister_event under ust_mutex).
 *
 * The translated descriptors reference strings (provider, event and
 * field names) owned by the instrumented object: this is safe
 * because REMOVE notifications are synchronous — the descriptors are
 * torn down under the notification before the object may be
 * unloaded — and ust_mutex is the descriptor validity domain on the
 * tracer side.
 *
 * POC scope: static (non-variadic) events; field types supported by
 * LTTng-UST tracepoints: integers, pointers, bool, byte, floats,
 * UTF-8 strings, and arrays/sequences of integer elements. Variadic
 * events and dynamic types are not mapped for now (a future blob
 * field with a media type could carry them, encoded in a
 * self-describing encoding such as MessagePack: a dynamic value is
 * typed at the instrumentation call site and nowhere else, so a
 * schema-first encoding does not fit); events
 * containing unsupported types are skipped. Filter bytecode wiring
 * is a separate phase: events with a filter attached are discarded.
 * Serialization walks the description/payload with the simple
 * visitor-based iterators (future work: bytecode).
 */

struct lttng_ust_side_event {
	struct lttng_ust_event_desc parent;

	struct lttng_ust_tracepoint_class tp_class;
	struct lttng_ust_probe_desc probe_desc;
	const struct lttng_ust_event_desc *event_desc_array[1];
	struct side_event_description *side_desc;
	/*
	 * The state is what the side callbacks are registered against,
	 * and what a description hangs off. See
	 * side_event_state_description().
	 */
	struct side_event_state *side_state;
	struct lttng_ust_registered_probe *reg_probe;
	const int *loglevel_ptr;
	int loglevel;
	const char *model_emf_uri_ptr;
	const struct lttng_ust_event_field **fields;
	uint32_t nr_fields;
	struct cds_list_head node;
};

struct lttng_ust_side_registration {
	struct cds_list_head node;
	struct side_event_state **side_events;		/* Identity for REMOVE. */
	uint32_t nr_side_events;
	struct cds_list_head events;
};

/* Protected by the side notification delivery serialization. */
static CDS_LIST_HEAD(side_registration_list);

/*
 * Map side loglevels (syslog-style 0-7) to LTTng-UST tracepoint
 * loglevels (0-14, TRACE_EMERG..TRACE_DEBUG).
 */
static const int side_loglevel_to_lttng[] = {
	[SIDE_LOGLEVEL_EMERG] = 0,
	[SIDE_LOGLEVEL_ALERT] = 1,
	[SIDE_LOGLEVEL_CRIT] = 2,
	[SIDE_LOGLEVEL_ERR] = 3,
	[SIDE_LOGLEVEL_WARNING] = 4,
	[SIDE_LOGLEVEL_NOTICE] = 5,
	[SIDE_LOGLEVEL_INFO] = 6,
	[SIDE_LOGLEVEL_DEBUG] = 14,
};

/*
 * Marker used as tp_class->probe_callback to recognize side-backed
 * event descriptors in register_event()/unregister_event().
 */
static
void lttng_ust_side_probe_marker(void)
{
}

static
size_t side_integer_alignof(uint16_t integer_size)
{
	switch (integer_size) {
	case 1:
		return lttng_ust_rb_alignof(uint8_t);
	case 2:
		return lttng_ust_rb_alignof(uint16_t);
	case 4:
		return lttng_ust_rb_alignof(uint32_t);
	case 8:
		return lttng_ust_rb_alignof(uint64_t);
	default:
		return 0;
	}
}

/*
 * Translate the attributes of a side event or type into LTTng
 * attributes. A side attribute key is a namespaced name: the namespace
 * is what precedes its last separator, and is empty when the key does
 * not have one. The attribute named @skip_key, if any, is left out: it
 * is described by the type itself rather than as an attribute.
 */
static
void side_translate_attributes_destroy(const struct lttng_ust_attributes *attributes)
{
	unsigned int i;

	if (!attributes)
		return;
	for (i = 0; i < attributes->nr_attributes; i++) {
		const struct lttng_ust_attribute *attr = attributes->attributes[i];

		if (!attr)
			continue;
		free((void *) attr->ns);
		free((void *) attr->name);
		if (attr->type == LTTNG_UST_ATTRIBUTE_TYPE_STRING)
			free((void *) attr->u.string_value);
		free((void *) attr);
	}
	free((void *) attributes->attributes);
	free((void *) attributes);
}

static
int side_translate_attribute_value(struct lttng_ust_attribute *attr,
		const struct side_attr_value *value)
{
	switch (side_enum_get(value->type)) {
	case SIDE_ATTR_TYPE_BOOL:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_BOOL;
		attr->u.bool_value = !!value->u.bool_value;
		break;
	case SIDE_ATTR_TYPE_U8:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_U64;
		attr->u.u64_value = value->u.integer_value.side_u8;
		break;
	case SIDE_ATTR_TYPE_U16:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_U64;
		attr->u.u64_value = value->u.integer_value.side_u16;
		break;
	case SIDE_ATTR_TYPE_U32:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_U64;
		attr->u.u64_value = value->u.integer_value.side_u32;
		break;
	case SIDE_ATTR_TYPE_U64:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_U64;
		attr->u.u64_value = value->u.integer_value.side_u64;
		break;
	case SIDE_ATTR_TYPE_S8:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_S64;
		attr->u.s64_value = value->u.integer_value.side_s8;
		break;
	case SIDE_ATTR_TYPE_S16:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_S64;
		attr->u.s64_value = value->u.integer_value.side_s16;
		break;
	case SIDE_ATTR_TYPE_S32:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_S64;
		attr->u.s64_value = value->u.integer_value.side_s32;
		break;
	case SIDE_ATTR_TYPE_S64:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_S64;
		attr->u.s64_value = value->u.integer_value.side_s64;
		break;
#if __HAVE_FLOAT32
	case SIDE_ATTR_TYPE_FLOAT_BINARY32:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_DOUBLE;
		attr->u.double_value = value->u.float_value.side_float_binary32;
		break;
#endif
#if __HAVE_FLOAT64
	case SIDE_ATTR_TYPE_FLOAT_BINARY64:
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_DOUBLE;
		attr->u.double_value = value->u.float_value.side_float_binary64;
		break;
#endif
	case SIDE_ATTR_TYPE_STRING:
	{
		char *str;

		if (value->u.string_value.unit_size != 1)
			return -1;
		str = strdup((const char *) side_ptr_get(value->u.string_value.p));
		if (!str)
			return -1;
		attr->type = LTTNG_UST_ATTRIBUTE_TYPE_STRING;
		attr->u.string_value = str;
		break;
	}
	default:
		return -1;
	}
	return 0;
}

static
const struct lttng_ust_attributes *side_translate_attributes(
		const struct side_attr *_attr, uint32_t nr_attr,
		const char *skip_key)
{
	const struct lttng_ust_attribute **entries = NULL;
	struct lttng_ust_attributes *attributes = NULL;
	unsigned int nr_entries = 0;
	uint32_t i;

	if (!nr_attr)
		return NULL;
	entries = zmalloc(nr_attr * sizeof(*entries));
	if (!entries)
		return NULL;
	for (i = 0; i < nr_attr; i++) {
		const struct side_attr *sattr = &_attr[i];
		struct lttng_ust_attribute *attr;
		char *utf8_key = NULL, *ns, *name;
		const char *sep;

		tracer_convert_string_to_utf8(side_ptr_get(sattr->key.p),
			sattr->key.unit_size, side_enum_get(sattr->key.byte_order),
			NULL, &utf8_key);
		if (!utf8_key)
			goto error;
		if (skip_key && !strcmp(utf8_key, skip_key)) {
			if (utf8_key != side_ptr_get(sattr->key.p))
				free(utf8_key);
			continue;
		}
		/* The namespace is what precedes the last separator. */
		sep = strrchr(utf8_key, '.');
		if (sep) {
			ns = strndup(utf8_key, sep - utf8_key);
			name = strdup(sep + 1);
		} else {
			ns = strdup("");
			name = strdup(utf8_key);
		}
		if (utf8_key != side_ptr_get(sattr->key.p))
			free(utf8_key);
		if (!ns || !name) {
			free(ns);
			free(name);
			goto error;
		}
		attr = zmalloc(sizeof(struct lttng_ust_attribute));
		if (!attr) {
			free(ns);
			free(name);
			goto error;
		}
		attr->struct_size = sizeof(struct lttng_ust_attribute);
		attr->ns = ns;
		attr->name = name;
		if (side_translate_attribute_value(attr, &sattr->value)) {
			free(ns);
			free(name);
			free(attr);
			goto error;
		}
		entries[nr_entries++] = attr;
	}
	if (!nr_entries) {
		free(entries);
		return NULL;
	}
	attributes = zmalloc(sizeof(struct lttng_ust_attributes));
	if (!attributes)
		goto error;
	attributes->nr_attributes = nr_entries;
	attributes->attributes = entries;
	return attributes;

error:
	if (entries) {
		unsigned int j;

		for (j = 0; j < nr_entries; j++) {
			const struct lttng_ust_attribute *a = entries[j];

			free((void *) a->ns);
			free((void *) a->name);
			if (a->type == LTTNG_UST_ATTRIBUTE_TYPE_STRING)
				free((void *) a->u.string_value);
			free((void *) a);
		}
		free(entries);
	}
	free(attributes);
	return NULL;
}

/* One boolean attribute. */
static
struct lttng_ust_attribute *side_bool_attribute(const char *ns, const char *name,
		bool value)
{
	struct lttng_ust_attribute *attr;

	attr = zmalloc(sizeof(struct lttng_ust_attribute));
	if (!attr)
		return NULL;
	attr->struct_size = sizeof(struct lttng_ust_attribute);
	attr->ns = strdup(ns);
	attr->name = strdup(name);
	if (!attr->ns || !attr->name) {
		free((void *) attr->ns);
		free((void *) attr->name);
		free(attr);
		return NULL;
	}
	attr->type = LTTNG_UST_ATTRIBUTE_TYPE_BOOL;
	attr->u.bool_value = value;
	return attr;
}

/*
 * Append @attr to @attributes, which may be empty, taking ownership of
 * both. Returns NULL on failure, having destroyed both, which leaves
 * the type without attributes rather than without a translation: they
 * describe it, they do not define it.
 */
static
const struct lttng_ust_attributes *side_attributes_append(
		const struct lttng_ust_attributes *attributes,
		struct lttng_ust_attribute *attr)
{
	const struct lttng_ust_attribute **entries, **new_entries;
	struct lttng_ust_attributes *new_attributes;
	unsigned int nr_entries = 0;

	if (!attr)
		goto error;
	if (attributes) {
		nr_entries = attributes->nr_attributes;
		entries = (const struct lttng_ust_attribute **) attributes->attributes;
		new_attributes = (struct lttng_ust_attributes *) attributes;
	} else {
		entries = NULL;
		new_attributes = zmalloc(sizeof(struct lttng_ust_attributes));
		if (!new_attributes)
			goto error;
	}
	new_entries = realloc(entries, (nr_entries + 1) * sizeof(*new_entries));
	if (!new_entries) {
		if (!attributes)
			free(new_attributes);
		goto error;
	}
	new_entries[nr_entries] = attr;
	new_attributes->attributes = new_entries;
	new_attributes->nr_attributes = nr_entries + 1;
	return new_attributes;

error:
	if (attr) {
		free((void *) attr->ns);
		free((void *) attr->name);
		free(attr);
	}
	side_translate_attributes_destroy(attributes);
	return NULL;
}

/* Whether the application described an attribute named @key. */
static
bool side_attr_has_key(const struct side_attr *_attr, uint32_t nr_attr,
		const char *key)
{
	uint32_t i;

	for (i = 0; i < nr_attr; i++) {
		const struct side_attr *attr = &_attr[i];
		char *utf8_str = NULL;
		bool cmp;

		tracer_convert_string_to_utf8(side_ptr_get(attr->key.p),
			attr->key.unit_size, side_enum_get(attr->key.byte_order),
			NULL, &utf8_str);
		if (!utf8_str)
			continue;
		cmp = strcmp(utf8_str, key);
		if (utf8_str != side_ptr_get(attr->key.p))
			free(utf8_str);
		if (!cmp)
			return true;
	}
	return false;
}

/*
 * The attributes of a field which the translation synthesizes rather
 * than one the application described: the length of a sequence and the
 * selector of a variant, which CTF requires to precede the field they
 * belong to. Such a field is recorded and decoded like any other one;
 * the attribute says that a reader showing the payload to a person can
 * leave it out.
 */
static
const struct lttng_ust_attributes *side_hidden_attributes(void)
{
	return side_attributes_append(NULL,
		side_bool_attribute("lttng.visibility", "hidden", true));
}

/* A blob is an array or a VLA of bytes. */
static
bool side_type_is_byte(const struct side_type *type_desc)
{
	return side_enum_get(type_desc->type) == SIDE_TYPE_BYTE;
}

/*
 * Media type of a blob, described by the standard "std.blob.media-type"
 * attribute of its array or VLA type. Returns NULL if the attribute is
 * absent, which describes a blob without media type.
 */
static
char *side_attr_media_type(const struct side_attr *_attr, uint32_t nr_attr)
{
	uint32_t i;

	for (i = 0; i < nr_attr; i++) {
		const struct side_attr *attr = &_attr[i];
		char *utf8_str = NULL, *media_type = NULL;
		bool cmp;

		tracer_convert_string_to_utf8(side_ptr_get(attr->key.p), attr->key.unit_size,
			side_enum_get(attr->key.byte_order), NULL, &utf8_str);
		cmp = strcmp(utf8_str, "std.blob.media-type");
		if (utf8_str != side_ptr_get(attr->key.p))
			free(utf8_str);
		if (cmp)
			continue;
		if (side_enum_get(attr->value.type) != SIDE_ATTR_TYPE_STRING)
			return NULL;
		if (attr->value.u.string_value.unit_size != 1)
			return NULL;
		media_type = strdup((const char *)
			side_ptr_get(attr->value.u.string_value.p));
		return media_type;
	}
	return NULL;
}

/*
 * Alignment of a side type, in bytes, as used to serialize it into the
 * ring buffer. A structure is aligned on the largest alignment of the
 * fields it contains, which is the alignment a trace reader applies
 * when the metadata does not state an explicit alignment. This is all
 * a no-op on architectures with efficient unaligned accesses, where
 * lttng_ust_rb_alignof() evaluates to 1.
 *
 * Note for when variants are eventually mapped: their alignment is a
 * CTF special case, expressed with a padding field preceding the
 * variant rather than by the alignment of its options.
 *
 * Returns 0 for the types which are not serialized.
 */
static
size_t side_struct_alignof(const struct side_type_struct *side_struct);

static
size_t side_type_alignof(const struct side_type *type_desc)
{
	switch (side_enum_get(type_desc->type)) {
	case SIDE_TYPE_BOOL:
		return side_integer_alignof(type_desc->u.side_bool.bool_size);
	case SIDE_TYPE_U8:		/* Fall-through. */
	case SIDE_TYPE_U16:		/* Fall-through. */
	case SIDE_TYPE_U32:		/* Fall-through. */
	case SIDE_TYPE_U64:		/* Fall-through. */
	case SIDE_TYPE_S8:		/* Fall-through. */
	case SIDE_TYPE_S16:		/* Fall-through. */
	case SIDE_TYPE_S32:		/* Fall-through. */
	case SIDE_TYPE_S64:		/* Fall-through. */
	case SIDE_TYPE_POINTER:
		return side_integer_alignof(type_desc->u.side_integer.integer_size);
	case SIDE_TYPE_BYTE:
		return lttng_ust_rb_alignof(uint8_t);
	case SIDE_TYPE_FLOAT_BINARY32:
		return lttng_ust_rb_alignof(float);
	case SIDE_TYPE_FLOAT_BINARY64:
		return lttng_ust_rb_alignof(double);
	case SIDE_TYPE_STRING_UTF8:
		return lttng_ust_rb_alignof(char);
	case SIDE_TYPE_ARRAY:
		return side_type_alignof(side_ptr_rel_get(
			side_ptr_rel_get(type_desc->u.side_array)->elem_type));
	case SIDE_TYPE_VLA:
	{
		const struct side_type_vla *v = side_ptr_rel_get(type_desc->u.side_vla);
		size_t length_align, elem_align;

		length_align = side_type_alignof(side_ptr_rel_get(v->length_type));
		elem_align = side_type_alignof(side_ptr_rel_get(v->elem_type));
		return length_align > elem_align ? length_align : elem_align;
	}
	case SIDE_TYPE_STRUCT:
		return side_struct_alignof(side_ptr_rel_get(type_desc->u.side_struct));
	case SIDE_TYPE_ENUM:
		/* An enumeration is aligned like its container. */
		return side_type_alignof(side_ptr_rel_get(type_desc->u.side_enum.elem_type));
	case SIDE_TYPE_VARIANT:
	{
		const struct side_type_variant *v = side_ptr_rel_get(type_desc->u.side_variant);

		/*
		 * CTF 2 gives a variant an alignment requirement of
		 * one, so it does not raise the alignment of what
		 * contains it, and its selected option is aligned by
		 * its own type against the offset it is recorded at.
		 * What this field does contribute is its selector,
		 * which precedes it as a field of its own.
		 */
		return side_type_alignof(&v->selector);
	}

	/*
	 * A gathered value is recorded like the value it wraps, so it
	 * is aligned like it as well.
	 */
	case SIDE_TYPE_GATHER_BOOL:
		return side_integer_alignof(
			type_desc->u.side_gather.u.side_bool.type.bool_size);
	case SIDE_TYPE_GATHER_INTEGER:	/* Fall-through. */
	case SIDE_TYPE_GATHER_POINTER:
		return side_integer_alignof(
			type_desc->u.side_gather.u.side_integer.type.integer_size);
	case SIDE_TYPE_GATHER_BYTE:
		return lttng_ust_rb_alignof(uint8_t);
	case SIDE_TYPE_GATHER_FLOAT:
		switch (type_desc->u.side_gather.u.side_float.type.float_size) {
		case 4:
			return lttng_ust_rb_alignof(float);
		case 8:
			return lttng_ust_rb_alignof(double);
		default:
			return 0;
		}
	case SIDE_TYPE_GATHER_STRING:
		return lttng_ust_rb_alignof(char);
	case SIDE_TYPE_GATHER_ENUM:
		return side_type_alignof(side_ptr_rel_get(
			type_desc->u.side_gather.u.side_enum.elem_type));
	case SIDE_TYPE_GATHER_STRUCT:
		return side_struct_alignof(side_ptr_rel_get(
			type_desc->u.side_gather.u.side_struct.type));
	case SIDE_TYPE_GATHER_ARRAY:
		return side_type_alignof(side_ptr_rel_get(
			type_desc->u.side_gather.u.side_array.type.elem_type));
	case SIDE_TYPE_GATHER_VLA:
	{
		const struct side_type_vla *v =
			&type_desc->u.side_gather.u.side_vla.type;
		size_t length_align, elem_align;

		length_align = side_type_alignof(side_ptr_rel_get(v->length_type));
		elem_align = side_type_alignof(side_ptr_rel_get(v->elem_type));
		return length_align > elem_align ? length_align : elem_align;
	}
	default:
		return 0;
	}
}

static
size_t side_struct_alignof(const struct side_type_struct *side_struct)
{
	uint32_t i, nr_fields = side_array_length(&side_struct->fields);
	size_t align = 1;

	for (i = 0; i < nr_fields; i++) {
		const struct side_event_field *f = side_array_rel_at(&side_struct->fields, i);
		size_t field_align = side_type_alignof(&f->side_type);

		if (field_align > align)
			align = field_align;
	}
	return align;
}

/*
 * The alignment of the payload of an event is a property of its
 * description, not of the values a given instance carries: a reader
 * derives it from the field types, where a sequence is aligned on its
 * elements whether it has any or not. Deriving it instead from the
 * fields actually recorded would under-align the payload of an
 * instance which records none of the fields the alignment comes from.
 */
static
size_t side_event_alignof(const struct side_event_description *desc)
{
	uint32_t i, nr_fields = side_array_length(&desc->fields);
	size_t align = 1;

	for (i = 0; i < nr_fields; i++) {
		const struct side_event_field *f = side_array_rel_at(&desc->fields, i);
		size_t field_align = side_type_alignof(&f->side_type);

		if (field_align > align)
			align = field_align;
	}
	return align;
}

/*
 * The trace records the values in the byte order they are emitted
 * with, and the description tells the reader which one that is. Both
 * helpers take the byte order of a side type and answer whether it
 * differs from the byte order the reader assumes for the trace, which
 * is the byte order of the host.
 */
static
bool side_type_reverse_byte_order(enum side_type_label_byte_order byte_order)
{
	return byte_order != SIDE_TYPE_BYTE_ORDER_HOST;
}

static
bool side_float_type_reverse_byte_order(enum side_type_label_byte_order byte_order)
{
	return byte_order != SIDE_TYPE_FLOAT_WORD_ORDER_HOST;
}

static
const struct lttng_ust_type_common *side_integer_type_to_lttng(
		uint16_t integer_size, uint16_t len_bits, bool signedness,
		bool reverse_byte_order,
		const struct side_attr *attr, uint32_t nr_attr,
		enum tracer_display_base default_base)
{
	struct lttng_ust_type_integer *t;
	unsigned int base;
	size_t align;

	align = side_integer_alignof(integer_size);
	if (!align)
		return NULL;
	/* Reject bit-packed integers for now. */
	if (len_bits && len_bits != (uint16_t) (integer_size * CHAR_BIT))
		return NULL;
	switch (get_attr_display_base(attr, nr_attr, default_base)) {
	case TRACER_DISPLAY_BASE_2:
		base = 2;
		break;
	case TRACER_DISPLAY_BASE_8:
		base = 8;
		break;
	case TRACER_DISPLAY_BASE_10:
		base = 10;
		break;
	case TRACER_DISPLAY_BASE_16:
		base = 16;
		break;
	default:
		return NULL;
	}
	t = zmalloc(sizeof(struct lttng_ust_type_integer));
	if (!t)
		return NULL;
	t->parent.type = lttng_ust_type_integer;
	t->struct_size = sizeof(struct lttng_ust_type_integer);
	t->size = integer_size * CHAR_BIT;
	t->alignment = align * CHAR_BIT;
	t->attributes = side_translate_attributes(attr, nr_attr, NULL);
	t->signedness = signedness;
	/*
	 * Values are serialized in the byte order they are emitted with:
	 * the description carries that byte order, and the reader of the
	 * trace converts.
	 */
	t->reverse_byte_order = reverse_byte_order;
	t->base = base;
	return &t->parent;
}

/*
 * Event description translation, driven by the description visitor.
 */
enum side_translate_state {
	SIDE_TRANSLATE_TOPLEVEL,
	SIDE_TRANSLATE_IN_ARRAY,
	SIDE_TRANSLATE_IN_VLA_LENGTH,
	SIDE_TRANSLATE_IN_VLA_ELEM,
	SIDE_TRANSLATE_IN_ENUM,
	SIDE_TRANSLATE_IN_VARIANT_SELECTOR,
	SIDE_TRANSLATE_IN_VARIANT_OPTION,
};

/*
 * Structures nest field scopes: their fields are collected into their
 * own array, which becomes the field array of the structure type. The
 * enclosing field and state are saved by the scope, because the field
 * and element callbacks of the members overwrite them.
 */
#define SIDE_TRANSLATE_MAX_NESTING	8

struct side_translate_scope {
	const struct lttng_ust_event_field **fields;
	unsigned int nr_fields;
	const struct side_event_field *field;
	enum side_translate_state state;
	/*
	 * The translation of an array, of a sequence and of an
	 * enumeration keeps what it is building in the context, and the
	 * members of a compound type nested within one of them build
	 * their own. Saved here for as long as they do.
	 */
	const struct lttng_ust_type_common *elem_type;
	const struct lttng_ust_type_common *vla_length_type;
	const struct lttng_ust_type_common *enum_container_type;
	uint32_t array_length;
};

struct side_translate_ctx {
	struct lttng_ust_side_event *se;
	const struct side_event_description *sdesc;
	const struct side_event_field *field;
	enum side_translate_state state;
	const struct lttng_ust_type_common *elem_type;
	const struct lttng_ust_type_common *vla_length_type;
	uint32_t array_length;
	bool fail;
	struct side_translate_scope scopes[SIDE_TRANSLATE_MAX_NESTING];
	unsigned int nesting;	/* 0: event payload, > 0: within structures. */
	/*
	 * Enumeration being translated: its container type is the only
	 * type it holds, so a single saved context is enough.
	 */
	const struct lttng_ust_type_common *enum_container_type;
	const struct side_event_field *enum_field;
	enum side_translate_state enum_state;
	/*
	 * Variant being translated: the selector type it describes, and
	 * the name of the option being translated. Variants do not nest
	 * within their own selector or options, so a single one is
	 * enough.
	 */
	const struct lttng_ust_type_common *variant_selector_type;
	char variant_option_name[LTTNG_UST_ABI_SYM_NAME_LEN];
};

static
void side_translate_fields_destroy(const struct lttng_ust_event_field **fields,
		unsigned int nr_fields);

static
void side_translate_type_destroy(const struct lttng_ust_type_common *type)
{
	if (!type)
		return;
	side_translate_attributes_destroy(lttng_ust_type_attributes(type));
	switch (type->type) {
	case lttng_ust_type_array:
	{
		const struct lttng_ust_type_array *a =
			caa_container_of(type, const struct lttng_ust_type_array, parent);
		side_translate_type_destroy(a->elem_type);
		break;
	}
	case lttng_ust_type_sequence:
	{
		const struct lttng_ust_type_sequence *s =
			caa_container_of(type, const struct lttng_ust_type_sequence, parent);
		side_translate_type_destroy(s->elem_type);
		break;
	}
	case lttng_ust_type_struct:
	{
		const struct lttng_ust_type_struct *s =
			caa_container_of(type, const struct lttng_ust_type_struct, parent);

		side_translate_fields_destroy(
			(const struct lttng_ust_event_field **) s->fields,
			s->nr_fields);
		break;
	}
	case lttng_ust_type_fixed_length_blob:
	{
		const struct lttng_ust_type_fixed_length_blob *b =
			caa_container_of(type, const struct lttng_ust_type_fixed_length_blob, parent);

		free((void *) b->media_type);
		break;
	}
	case lttng_ust_type_variable_length_blob:
	{
		const struct lttng_ust_type_variable_length_blob *b =
			caa_container_of(type, const struct lttng_ust_type_variable_length_blob, parent);

		free((void *) b->media_type);
		break;
	}
	case lttng_ust_type_variant:
	{
		const struct lttng_ust_type_variant *v =
			caa_container_of(type, const struct lttng_ust_type_variant, parent);

		side_translate_fields_destroy(
			(const struct lttng_ust_event_field **) v->choices,
			v->nr_choices);
		break;
	}
	case lttng_ust_type_enum:
	{
		const struct lttng_ust_type_enum *e =
			caa_container_of(type, const struct lttng_ust_type_enum, parent);
		const struct lttng_ust_enum_desc *desc = e->desc;

		side_translate_type_destroy(e->container_type);
		if (desc) {
			unsigned int i;

			for (i = 0; i < desc->nr_entries; i++) {
				const struct lttng_ust_enum_entry *entry = desc->entries[i];

				if (!entry)
					continue;
				free((void *) entry->string);
				free((void *) entry);
			}
			free((void *) desc->entries);
			free((void *) desc->name);
			free((void *) desc);
		}
		break;
	}
	default:
		break;
	}
	free((void *) type);
}

static
void side_translate_fields_destroy(const struct lttng_ust_event_field **fields,
		unsigned int nr_fields)
{
	unsigned int i;

	if (!fields)
		return;
	for (i = 0; i < nr_fields; i++) {
		const struct lttng_ust_event_field *f = fields[i];

		if (!f)
			continue;
		side_translate_type_destroy(f->type);
		side_translate_attributes_destroy(f->attributes);
		free((void *) f->name);
		free((void *) f);
	}
	free(fields);
}

/*
 * Append a field to the scope being translated. A synthesized field is
 * one the application did not describe: the length of a sequence and
 * the selector of a variant, which CTF requires to precede the field
 * they belong to. It is not a field a filter can name, and it carries
 * the attribute which says a reader may leave it out.
 */
static
bool side_translate_append_field(struct side_translate_ctx *ctx,
		const char *name, const struct lttng_ust_type_common *type,
		bool synthesized)
{
	struct side_translate_scope *scope = &ctx->scopes[ctx->nesting];
	const struct lttng_ust_attributes *attributes = NULL;
	const struct lttng_ust_event_field **new_fields;
	struct lttng_ust_event_field *f;
	char *name_copy;

	if (!type)
		goto fail;
	name_copy = strdup(name);
	if (!name_copy)
		goto fail_free_type;
	if (synthesized) {
		attributes = side_hidden_attributes();
		if (!attributes)
			goto fail_free_name;
	}
	f = zmalloc(sizeof(struct lttng_ust_event_field));
	if (!f)
		goto fail_free_attributes;
	new_fields = realloc(scope->fields,
		(scope->nr_fields + 1) * sizeof(*new_fields));
	if (!new_fields)
		goto fail_free_field;
	scope->fields = new_fields;
	f->struct_size = sizeof(struct lttng_ust_event_field);
	f->name = name_copy;
	f->type = type;
	f->nowrite = 0;
	f->nofilter = synthesized;
	f->attributes = attributes;
	scope->fields[scope->nr_fields++] = f;
	return true;

fail_free_field:
	free(f);
fail_free_attributes:
	side_translate_attributes_destroy(attributes);
fail_free_name:
	free(name_copy);
fail_free_type:
	side_translate_type_destroy(type);
	type = NULL;
fail:
	ctx->fail = true;
	return false;
}

static
void side_translate_set_fail(struct side_translate_ctx *ctx)
{
	ctx->fail = true;
}

/*
 * Add a translated type to the enclosing context: a field of the
 * current field scope, or the element or length type of the array or
 * VLA being translated.
 */
static
void side_translate_commit_type(struct side_translate_ctx *ctx,
		const struct lttng_ust_type_common *type,
		const struct side_event_field *field)
{
	if (ctx->fail) {
		side_translate_type_destroy(type);
		return;
	}
	switch (ctx->state) {
	case SIDE_TRANSLATE_TOPLEVEL:
		if (!field) {
			side_translate_type_destroy(type);
			ctx->fail = true;
			break;
		}
		(void) side_translate_append_field(ctx,
			side_ptr_rel_get(field->field_name), type, false);
		break;
	case SIDE_TRANSLATE_IN_ARRAY:
	case SIDE_TRANSLATE_IN_VLA_ELEM:
		if (ctx->elem_type) {
			side_translate_type_destroy(type);
			ctx->fail = true;
			break;
		}
		if (!type) {
			ctx->fail = true;
			break;
		}
		ctx->elem_type = type;
		break;
	case SIDE_TRANSLATE_IN_VLA_LENGTH:
		if (ctx->vla_length_type) {
			side_translate_type_destroy(type);
			ctx->fail = true;
			break;
		}
		if (!type) {
			ctx->fail = true;
			break;
		}
		ctx->vla_length_type = type;
		break;
	case SIDE_TRANSLATE_IN_ENUM:
		if (ctx->enum_container_type) {
			side_translate_type_destroy(type);
			ctx->fail = true;
			break;
		}
		if (!type) {
			ctx->fail = true;
			break;
		}
		ctx->enum_container_type = type;
		break;
	case SIDE_TRANSLATE_IN_VARIANT_SELECTOR:
		if (ctx->variant_selector_type) {
			side_translate_type_destroy(type);
			ctx->fail = true;
			break;
		}
		if (!type) {
			ctx->fail = true;
			break;
		}
		ctx->variant_selector_type = type;
		break;
	case SIDE_TRANSLATE_IN_VARIANT_OPTION:
		/* An option is a choice of the variant, named after its label. */
		(void) side_translate_append_field(ctx, ctx->variant_option_name,
			type, false);
		break;
	}
}

static
void side_translate_integer_common(struct side_translate_ctx *ctx,
		const struct lttng_ust_type_common *type)
{
	side_translate_commit_type(ctx, type, ctx->field);
}

static
void side_translate_integer_type(const struct side_type_integer *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	const struct lttng_ust_type_common *type;

	type = side_integer_type_to_lttng(t->integer_size, t->len_bits,
		t->signedness, side_type_reverse_byte_order(side_enum_get(t->byte_order)),
		side_array_elements(&t->attributes),
		side_array_length(&t->attributes),
		TRACER_DISPLAY_BASE_10);
	if (ctx->state == SIDE_TRANSLATE_IN_VLA_LENGTH && t->signedness) {
		/* Sequence lengths must be unsigned. */
		side_translate_type_destroy(type);
		type = NULL;
	}
	side_translate_integer_common(ctx, type);
}

static
void side_translate_pointer_type(const struct side_type_integer *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	const struct lttng_ust_type_common *type;

	type = side_integer_type_to_lttng(t->integer_size, t->len_bits,
		t->signedness, side_type_reverse_byte_order(side_enum_get(t->byte_order)),
		side_array_elements(&t->attributes),
		side_array_length(&t->attributes),
		TRACER_DISPLAY_BASE_16);
	side_translate_integer_common(ctx, type);
}

static
void side_translate_byte_type(const struct side_type_byte *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	const struct lttng_ust_type_common *type;

	/* A single byte has no byte order. */
	type = side_integer_type_to_lttng(1, 0, false, false,
		side_array_elements(&t->attributes),
		side_array_length(&t->attributes),
		TRACER_DISPLAY_BASE_16);
	side_translate_integer_common(ctx, type);
}

static
void side_translate_bool_type(const struct side_type_bool *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	const struct lttng_ust_type_common *type;

	if (ctx->fail)
		return;
	type = side_integer_type_to_lttng(t->bool_size, t->len_bits, false,
		side_type_reverse_byte_order(side_enum_get(t->byte_order)),
		side_array_elements(&t->attributes),
		side_array_length(&t->attributes),
		TRACER_DISPLAY_BASE_10);
	side_translate_commit_type(ctx, type, ctx->field);
}

static
void side_translate_float_type(const struct side_type_float *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	struct lttng_ust_type_float *type;
	unsigned int exp_dig, mant_dig;
	size_t align;

	if (ctx->fail)
		return;
	switch (t->float_size) {
	case 4:
		exp_dig = 8;
		mant_dig = 24;
		align = lttng_ust_rb_alignof(float);
		break;
	case 8:
		exp_dig = 11;
		mant_dig = 53;
		align = lttng_ust_rb_alignof(double);
		break;
	default:
		ctx->fail = true;
		return;
	}
	type = zmalloc(sizeof(struct lttng_ust_type_float));
	if (!type) {
		ctx->fail = true;
		return;
	}
	type->parent.type = lttng_ust_type_float;
	type->struct_size = sizeof(struct lttng_ust_type_float);
	type->exp_dig = exp_dig;
	type->mant_dig = mant_dig;
	type->alignment = align * CHAR_BIT;
	type->reverse_byte_order = side_float_type_reverse_byte_order(side_enum_get(t->byte_order));
	type->attributes = side_translate_attributes(
		side_array_elements(&t->attributes),
		side_array_length(&t->attributes), NULL);
	side_translate_commit_type(ctx, &type->parent, ctx->field);
}

static
void side_translate_string_type(const struct side_type_string *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	struct lttng_ust_type_string *type;

	if (ctx->fail)
		return;
	if (t->unit_size != 1) {
		ctx->fail = true;
		return;
	}
	type = zmalloc(sizeof(struct lttng_ust_type_string));
	if (!type) {
		ctx->fail = true;
		return;
	}
	type->parent.type = lttng_ust_type_string;
	type->struct_size = sizeof(struct lttng_ust_type_string);
	type->encoding = lttng_ust_string_encoding_UTF8;
	type->attributes = side_translate_attributes(
		side_array_elements(&t->attributes),
		side_array_length(&t->attributes), NULL);
	side_translate_commit_type(ctx, &type->parent, ctx->field);
}

static
void side_translate_before_field(const struct side_event_field *item_desc, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	ctx->field = item_desc;
}

static
void side_translate_after_field(const struct side_event_field *item_desc __attribute__((unused)),
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	ctx->field = NULL;
}

/*
 * Side enumerations are anonymous: name them after the path of the
 * field they describe, which is unique within an event, prefixed by
 * the provider and event names. Characters which are not valid in an
 * identifier are replaced.
 */
static
char *side_translate_enum_name(struct side_translate_ctx *ctx,
		const struct side_event_field *field)
{
	char name[LTTNG_UST_ABI_SYM_NAME_LEN];
	unsigned int i;
	size_t len;
	int ret;

	ret = snprintf(name, sizeof(name), "%s_%s",
		side_ptr_rel_get(ctx->sdesc->provider_name),
		side_ptr_rel_get(ctx->sdesc->event_name));
	if (ret < 0 || ret >= (int) sizeof(name))
		return NULL;
	/* Enclosing structures, outermost first. */
	for (i = 1; i <= ctx->nesting && i < SIDE_TRANSLATE_MAX_NESTING; i++) {
		const struct side_event_field *enclosing = ctx->scopes[i].field;

		if (!enclosing)
			continue;
		len = strlen(name);
		ret = snprintf(name + len, sizeof(name) - len, "_%s",
			side_ptr_rel_get(enclosing->field_name));
		if (ret < 0 || ret >= (int) (sizeof(name) - len))
			return NULL;
	}
	if (field) {
		len = strlen(name);
		ret = snprintf(name + len, sizeof(name) - len, "_%s",
			side_ptr_rel_get(field->field_name));
		if (ret < 0 || ret >= (int) (sizeof(name) - len))
			return NULL;
	}
	for (i = 0; name[i]; i++) {
		char c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9') || c == '_'))
			name[i] = '_';
	}
	return strdup(name);
}

static
void side_translate_before_enum(const struct side_type_enum *t __attribute__((unused)),
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->fail)
		return;
	if (ctx->state == SIDE_TRANSLATE_IN_ENUM || ctx->enum_container_type) {
		/* Enumerations do not nest. */
		ctx->fail = true;
		return;
	}
	ctx->enum_field = ctx->field;
	ctx->enum_state = ctx->state;
	ctx->state = SIDE_TRANSLATE_IN_ENUM;
}

static
void side_translate_after_enum_mappings(const struct side_enum_mappings *mappings,
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	const struct lttng_ust_enum_entry **entries = NULL;
	const struct lttng_ust_type_common *container;
	struct lttng_ust_enum_desc *desc = NULL;
	const struct side_event_field *field;
	struct lttng_ust_type_enum *type;
	uint32_t i, nr_mappings = 0;
	char *name = NULL;
	bool signedness;

	container = ctx->enum_container_type;
	field = ctx->enum_field;
	ctx->enum_container_type = NULL;
	ctx->enum_field = NULL;
	ctx->state = ctx->enum_state;
	ctx->field = field;
	if (ctx->fail)
		goto fail;
	/* The container of an enumeration is an integer. */
	if (!container || container->type != lttng_ust_type_integer)
		goto fail;
	signedness = caa_container_of(container,
		const struct lttng_ust_type_integer, parent)->signedness;
	nr_mappings = side_array_length(&mappings->mappings);
	name = side_translate_enum_name(ctx, field);
	if (!name)
		goto fail;
	entries = zmalloc(nr_mappings * sizeof(*entries));
	if (nr_mappings && !entries)
		goto fail;
	for (i = 0; i < nr_mappings; i++) {
		const struct side_enum_mapping *m = side_array_at(&mappings->mappings, i);
		struct lttng_ust_enum_entry *entry;
		char *label;

		if (m->label.unit_size != 1)
			goto fail;
		label = strdup((const char *) side_ptr_get(m->label.p));
		if (!label)
			goto fail;
		entry = zmalloc(sizeof(struct lttng_ust_enum_entry));
		if (!entry) {
			free(label);
			goto fail;
		}
		entry->struct_size = sizeof(struct lttng_ust_enum_entry);
		entry->start.value = (unsigned long long) m->range_begin;
		entry->start.signedness = signedness;
		entry->end.value = (unsigned long long) m->range_end;
		entry->end.signedness = signedness;
		entry->string = label;
		entries[i] = entry;
	}
	desc = zmalloc(sizeof(struct lttng_ust_enum_desc));
	if (!desc)
		goto fail;
	desc->struct_size = sizeof(struct lttng_ust_enum_desc);
	desc->name = name;
	desc->entries = entries;
	desc->nr_entries = nr_mappings;
	desc->probe_desc = &ctx->se->probe_desc;
	type = zmalloc(sizeof(struct lttng_ust_type_enum));
	if (!type)
		goto fail;
	type->parent.type = lttng_ust_type_enum;
	type->struct_size = sizeof(struct lttng_ust_type_enum);
	type->desc = desc;
	type->container_type = container;
	type->attributes = side_translate_attributes(
		side_array_elements(&mappings->attributes),
		side_array_length(&mappings->attributes), NULL);
	/*
	 * The labels of an enumeration are what its values mean, and the
	 * number a label translates is a detail of the description. Ask a
	 * reader to show the label alone, unless the application asked
	 * for something else: an application which wants the number as
	 * well describes "lttng.fmt.print-value" itself.
	 */
	if (!side_attr_has_key(side_array_elements(&mappings->attributes),
			side_array_length(&mappings->attributes),
			"lttng.fmt.print-value")) {
		type->attributes = side_attributes_append(type->attributes,
			side_bool_attribute("lttng.fmt", "print-value", false));
	}
	side_translate_commit_type(ctx, &type->parent, field);
	return;

fail:
	if (entries) {
		for (i = 0; i < nr_mappings; i++) {
			if (!entries[i])
				continue;
			free((void *) entries[i]->string);
			free((void *) entries[i]);
		}
		free(entries);
	}
	free(desc);
	free(name);
	side_translate_type_destroy(container);
	ctx->fail = true;
}

static
void side_translate_after_enum(const struct side_type_enum *t, void *priv)
{
	side_translate_after_enum_mappings(side_ptr_get(t->mappings), priv);
}

/*
 * A side variant carries its own selector: its value travels with the
 * variant argument, and its options select on integer ranges. CTF
 * describes a variant by another field, which has to be an
 * enumeration, and names each choice after one of its labels.
 *
 * Translate the options into an enumeration, one label per option,
 * emit it as a hidden field preceding the variant, and name the
 * choices of the variant after those labels. The selector value is
 * written into that hidden field when the event is emitted.
 */
static
int side_variant_option_name(char *name, size_t len, uint32_t option_index)
{
	int ret;

	ret = snprintf(name, len, "option_%u", option_index);
	if (ret < 0 || ret >= (int) len)
		return -1;
	return 0;
}

static
const struct lttng_ust_type_common *side_variant_selector_enum(
		struct side_translate_ctx *ctx,
		const struct side_type_variant *v,
		const struct lttng_ust_type_common *container)
{
	const struct lttng_ust_enum_entry **entries = NULL;
	uint32_t i, nr_options = side_array_length(&v->options);
	struct lttng_ust_enum_desc *desc = NULL;
	struct lttng_ust_type_enum *type = NULL;
	char *name = NULL;
	bool signedness;

	if (!container || container->type != lttng_ust_type_integer)
		goto error;
	signedness = caa_container_of(container,
		const struct lttng_ust_type_integer, parent)->signedness;
	name = side_translate_enum_name(ctx, ctx->field);
	if (!name)
		goto error;
	entries = zmalloc(nr_options * sizeof(*entries));
	if (nr_options && !entries)
		goto error;
	for (i = 0; i < nr_options; i++) {
		const struct side_variant_option *option = side_array_rel_at(&v->options, i);
		char label[LTTNG_UST_ABI_SYM_NAME_LEN];
		struct lttng_ust_enum_entry *entry;
		char *label_copy;

		if (side_variant_option_name(label, sizeof(label), i))
			goto error;
		label_copy = strdup(label);
		if (!label_copy)
			goto error;
		entry = zmalloc(sizeof(struct lttng_ust_enum_entry));
		if (!entry) {
			free(label_copy);
			goto error;
		}
		entry->struct_size = sizeof(struct lttng_ust_enum_entry);
		entry->start.value = (unsigned long long) option->range_begin;
		entry->start.signedness = signedness;
		entry->end.value = (unsigned long long) option->range_end;
		entry->end.signedness = signedness;
		entry->string = label_copy;
		entries[i] = entry;
	}
	desc = zmalloc(sizeof(struct lttng_ust_enum_desc));
	if (!desc)
		goto error;
	desc->struct_size = sizeof(struct lttng_ust_enum_desc);
	desc->name = name;
	desc->entries = entries;
	desc->nr_entries = nr_options;
	desc->probe_desc = &ctx->se->probe_desc;
	type = zmalloc(sizeof(struct lttng_ust_type_enum));
	if (!type)
		goto error;
	type->parent.type = lttng_ust_type_enum;
	type->struct_size = sizeof(struct lttng_ust_type_enum);
	type->desc = desc;
	type->container_type = container;
	return &type->parent;

error:
	if (entries) {
		for (i = 0; i < nr_options; i++) {
			if (!entries[i])
				continue;
			free((void *) entries[i]->string);
			free((void *) entries[i]);
		}
		free(entries);
	}
	free(desc);
	free(name);
	return NULL;
}

static
void side_translate_before_variant(const struct side_type_variant *v __attribute__((unused)),
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	struct side_translate_scope *scope;

	if (ctx->fail)
		return;
	if (ctx->state != SIDE_TRANSLATE_TOPLEVEL || !ctx->field) {
		/* Only a field of an event or of a structure. */
		ctx->fail = true;
		return;
	}
	if (ctx->nesting + 1 >= SIDE_TRANSLATE_MAX_NESTING) {
		ctx->fail = true;
		return;
	}
	scope = &ctx->scopes[++ctx->nesting];
	memset(scope, 0, sizeof(*scope));
	scope->field = ctx->field;
	scope->state = ctx->state;
	/*
	 * The selector and the options overwrite what the enclosing
	 * array, sequence or enumeration is building: save it for the
	 * end of this scope.
	 */
	scope->elem_type = ctx->elem_type;
	scope->vla_length_type = ctx->vla_length_type;
	scope->enum_container_type = ctx->enum_container_type;
	scope->array_length = ctx->array_length;
	ctx->elem_type = NULL;
	ctx->vla_length_type = NULL;
	ctx->enum_container_type = NULL;
	ctx->array_length = 0;
	ctx->variant_selector_type = NULL;
	ctx->state = SIDE_TRANSLATE_IN_VARIANT_SELECTOR;
}

static
void side_translate_after_variant_selector(const struct side_type *selector __attribute__((unused)),
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->state != SIDE_TRANSLATE_IN_VARIANT_SELECTOR)
		ctx->fail = true;
	ctx->state = SIDE_TRANSLATE_IN_VARIANT_OPTION;
}

static
void side_translate_before_option(const struct side_variant_option *option __attribute__((unused)),
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	struct side_translate_scope *scope;

	if (ctx->fail)
		return;
	if (ctx->state != SIDE_TRANSLATE_IN_VARIANT_OPTION || !ctx->nesting) {
		ctx->fail = true;
		return;
	}
	scope = &ctx->scopes[ctx->nesting];
	if (side_variant_option_name(ctx->variant_option_name,
			sizeof(ctx->variant_option_name), scope->nr_fields))
		ctx->fail = true;
}

static
void side_translate_after_variant(const struct side_type_variant *v, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	const struct lttng_ust_event_field **choices;
	const struct lttng_ust_type_common *selector_enum = NULL;
	const struct side_event_field *field;
	struct side_translate_scope *scope;
	struct lttng_ust_type_variant *type;
	char selector_name[LTTNG_UST_ABI_SYM_NAME_LEN];
	unsigned int nr_choices;

	if (!ctx->nesting) {
		ctx->fail = true;
		return;
	}
	scope = &ctx->scopes[ctx->nesting--];
	choices = scope->fields;
	nr_choices = scope->nr_fields;
	field = scope->field;
	ctx->state = scope->state;
	ctx->field = field;
	/* Restore what this scope overwrote. */
	ctx->elem_type = scope->elem_type;
	ctx->vla_length_type = scope->vla_length_type;
	ctx->enum_container_type = scope->enum_container_type;
	ctx->array_length = scope->array_length;
	memset(scope, 0, sizeof(*scope));
	if (ctx->fail)
		goto fail;
	if (!nr_choices || nr_choices != side_array_length(&v->options))
		goto fail;
	selector_enum = side_variant_selector_enum(ctx, v, ctx->variant_selector_type);
	if (!selector_enum)
		goto fail;
	/* The selector type belongs to the enumeration from now on. */
	ctx->variant_selector_type = NULL;
	/* Hidden selector field, followed by the variant itself. */
	if (snprintf(selector_name, sizeof(selector_name), "_%s_selector",
			side_ptr_rel_get(field->field_name)) >= (int) sizeof(selector_name))
		goto fail;
	if (!side_translate_append_field(ctx, selector_name, selector_enum, true)) {
		selector_enum = NULL;
		goto fail;
	}
	selector_enum = NULL;
	type = zmalloc(sizeof(struct lttng_ust_type_variant));
	if (!type)
		goto fail;
	type->parent.type = lttng_ust_type_variant;
	type->struct_size = sizeof(struct lttng_ust_type_variant);
	type->tag_name = NULL;		/* Use previous field. */
	type->nr_choices = nr_choices;
	type->choices = choices;
	type->alignment = 0;
	type->attributes = side_translate_attributes(
		side_array_elements(&v->attributes),
		side_array_length(&v->attributes), NULL);
	side_translate_commit_type(ctx, &type->parent, field);
	return;

fail:
	side_translate_type_destroy(selector_enum);
	side_translate_type_destroy(ctx->variant_selector_type);
	ctx->variant_selector_type = NULL;
	side_translate_fields_destroy(choices, nr_choices);
	ctx->fail = true;
}

static
void side_translate_before_struct(const struct side_type_struct *side_struct __attribute__((unused)),
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	struct side_translate_scope *scope;

	if (ctx->fail)
		return;
	if (ctx->nesting + 1 >= SIDE_TRANSLATE_MAX_NESTING) {
		ctx->fail = true;
		return;
	}
	scope = &ctx->scopes[++ctx->nesting];
	memset(scope, 0, sizeof(*scope));
	/*
	 * The members overwrite the current field and use the field
	 * scope states: save them for after_struct.
	 */
	scope->field = ctx->field;
	scope->state = ctx->state;
	/*
	 * The members overwrite what the enclosing array, sequence or
	 * enumeration is building: save it for the end of this scope.
	 */
	scope->elem_type = ctx->elem_type;
	scope->vla_length_type = ctx->vla_length_type;
	scope->enum_container_type = ctx->enum_container_type;
	scope->array_length = ctx->array_length;
	ctx->elem_type = NULL;
	ctx->vla_length_type = NULL;
	ctx->enum_container_type = NULL;
	ctx->array_length = 0;
	ctx->state = SIDE_TRANSLATE_TOPLEVEL;
}

static
void side_translate_after_struct(const struct side_type_struct *side_struct, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	const struct lttng_ust_event_field **fields;
	const struct side_event_field *field;
	struct side_translate_scope *scope;
	struct lttng_ust_type_struct *type;
	unsigned int nr_fields;
	size_t align;

	if (!ctx->nesting) {
		ctx->fail = true;
		return;
	}
	scope = &ctx->scopes[ctx->nesting--];
	fields = scope->fields;
	nr_fields = scope->nr_fields;
	field = scope->field;
	/* Restore the enclosing context, overwritten by the members. */
	ctx->state = scope->state;
	ctx->field = field;
	/* Restore what the members of this scope overwrote. */
	ctx->elem_type = scope->elem_type;
	ctx->vla_length_type = scope->vla_length_type;
	ctx->enum_container_type = scope->enum_container_type;
	ctx->array_length = scope->array_length;
	memset(scope, 0, sizeof(*scope));
	if (ctx->fail)
		goto fail;
	align = side_struct_alignof(side_struct);
	if (!align)
		goto fail;
	type = zmalloc(sizeof(struct lttng_ust_type_struct));
	if (!type)
		goto fail;
	type->parent.type = lttng_ust_type_struct;
	type->struct_size = sizeof(struct lttng_ust_type_struct);
	type->nr_fields = nr_fields;
	type->fields = fields;
	type->alignment = align * CHAR_BIT;
	type->attributes = side_translate_attributes(
		side_array_elements(&side_struct->attributes),
		side_array_length(&side_struct->attributes), NULL);
	side_translate_commit_type(ctx, &type->parent, field);
	return;

fail:
	side_translate_fields_destroy(fields, nr_fields);
	ctx->fail = true;
}

static
void side_translate_before_array(const struct side_type_array *a, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->fail)
		return;
	if (ctx->state != SIDE_TRANSLATE_TOPLEVEL) {
		ctx->fail = true;
		return;
	}
	ctx->state = SIDE_TRANSLATE_IN_ARRAY;
	ctx->elem_type = NULL;
	ctx->array_length = a->length;
}

static
void side_translate_after_array(const struct side_type_array *a,
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	struct lttng_ust_type_array *type;

	if (ctx->state != SIDE_TRANSLATE_IN_ARRAY)
		ctx->fail = true;
	ctx->state = SIDE_TRANSLATE_TOPLEVEL;
	if (ctx->fail)
		goto fail;
	if (!ctx->elem_type)
		goto fail;
	if (side_type_is_byte(side_ptr_rel_get(a->elem_type))) {
		struct lttng_ust_type_fixed_length_blob *blob;

		/* An array of bytes is a blob. */
		blob = zmalloc(sizeof(struct lttng_ust_type_fixed_length_blob));
		if (!blob)
			goto fail;
		blob->parent.type = lttng_ust_type_fixed_length_blob;
		blob->struct_size = sizeof(struct lttng_ust_type_fixed_length_blob);
		blob->length = ctx->array_length;
		blob->media_type = side_attr_media_type(
			side_array_elements(&a->attributes),
			side_array_length(&a->attributes));
		blob->attributes = side_translate_attributes(
			side_array_elements(&a->attributes),
			side_array_length(&a->attributes),
			"std.blob.media-type");
		side_translate_type_destroy(ctx->elem_type);
		ctx->elem_type = NULL;
		(void) side_translate_append_field(ctx,
			side_ptr_rel_get(ctx->field->field_name), &blob->parent, false);
		return;
	}
	type = zmalloc(sizeof(struct lttng_ust_type_array));
	if (!type)
		goto fail;
	type->parent.type = lttng_ust_type_array;
	type->struct_size = sizeof(struct lttng_ust_type_array);
	type->elem_type = ctx->elem_type;
	type->length = ctx->array_length;
	type->alignment = 0;
	type->encoding = lttng_ust_string_encoding_none;
	type->attributes = side_translate_attributes(
		side_array_elements(&a->attributes),
		side_array_length(&a->attributes), NULL);
	ctx->elem_type = NULL;
	(void) side_translate_append_field(ctx,
		side_ptr_rel_get(ctx->field->field_name), &type->parent, false);
	return;

fail:
	side_translate_type_destroy(ctx->elem_type);
	ctx->elem_type = NULL;
	ctx->fail = true;
}

static
void side_translate_before_vla(const struct side_type_vla *v __attribute__((unused)),
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->fail)
		return;
	if (ctx->state != SIDE_TRANSLATE_TOPLEVEL) {
		ctx->fail = true;
		return;
	}
	ctx->state = SIDE_TRANSLATE_IN_VLA_LENGTH;
	ctx->elem_type = NULL;
	ctx->vla_length_type = NULL;
}

static
void side_translate_after_length_vla(const struct side_type_vla *v __attribute__((unused)),
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->state != SIDE_TRANSLATE_IN_VLA_LENGTH || !ctx->vla_length_type)
		ctx->fail = true;
	ctx->state = SIDE_TRANSLATE_IN_VLA_ELEM;
}

static
void side_translate_after_element_vla(const struct side_type_vla *v,
		void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;
	struct lttng_ust_type_sequence *type;
	char length_name[LTTNG_UST_ABI_SYM_NAME_LEN];

	if (ctx->state != SIDE_TRANSLATE_IN_VLA_ELEM)
		ctx->fail = true;
	ctx->state = SIDE_TRANSLATE_TOPLEVEL;
	if (ctx->fail)
		goto fail;
	if (!ctx->elem_type || !ctx->vla_length_type)
		goto fail;
	/* Hidden length field, followed by the sequence itself. */
	if (snprintf(length_name, sizeof(length_name), "_%s_length",
			side_ptr_rel_get(ctx->field->field_name)) >= (int) sizeof(length_name))
		goto fail;
	if (!side_translate_append_field(ctx, length_name,
			ctx->vla_length_type, true)) {
		ctx->vla_length_type = NULL;
		goto fail;
	}
	ctx->vla_length_type = NULL;
	if (side_type_is_byte(side_ptr_rel_get(v->elem_type))) {
		struct lttng_ust_type_variable_length_blob *blob;

		/* A VLA of bytes is a blob. */
		blob = zmalloc(sizeof(struct lttng_ust_type_variable_length_blob));
		if (!blob)
			goto fail;
		blob->parent.type = lttng_ust_type_variable_length_blob;
		blob->struct_size = sizeof(struct lttng_ust_type_variable_length_blob);
		blob->length_name = NULL;	/* Use previous field. */
		blob->media_type = side_attr_media_type(
			side_array_elements(&v->attributes),
			side_array_length(&v->attributes));
		blob->attributes = side_translate_attributes(
			side_array_elements(&v->attributes),
			side_array_length(&v->attributes),
			"std.blob.media-type");
		side_translate_type_destroy(ctx->elem_type);
		ctx->elem_type = NULL;
		(void) side_translate_append_field(ctx,
			side_ptr_rel_get(ctx->field->field_name), &blob->parent, false);
		return;
	}
	type = zmalloc(sizeof(struct lttng_ust_type_sequence));
	if (!type)
		goto fail;
	type->parent.type = lttng_ust_type_sequence;
	type->struct_size = sizeof(struct lttng_ust_type_sequence);
	type->length_name = NULL;	/* Use previous field. */
	type->elem_type = ctx->elem_type;
	type->alignment = 0;
	type->encoding = lttng_ust_string_encoding_none;
	type->attributes = side_translate_attributes(
		side_array_elements(&v->attributes),
		side_array_length(&v->attributes), NULL);
	ctx->elem_type = NULL;
	(void) side_translate_append_field(ctx,
		side_ptr_rel_get(ctx->field->field_name), &type->parent, false);
	return;

fail:
	side_translate_type_destroy(ctx->elem_type);
	ctx->elem_type = NULL;
	side_translate_type_destroy(ctx->vla_length_type);
	ctx->vla_length_type = NULL;
	ctx->fail = true;
}

/* Unsupported description elements: fail the translation. */
static
void side_translate_unsupported_null(const struct side_type_null *t __attribute__((unused)), void *priv)
{
	side_translate_set_fail((struct side_translate_ctx *) priv);
}

static
void side_translate_unsupported_dynamic(const struct side_type *t __attribute__((unused)), void *priv)
{
	side_translate_set_fail((struct side_translate_ctx *) priv);
}

#define SIDE_TRANSLATE_UNSUPPORTED(_name, _type)				\
static										\
void side_translate_unsupported_##_name(_type *t __attribute__((unused)),	\
		void *priv)							\
{										\
	side_translate_set_fail((struct side_translate_ctx *) priv);		\
}

SIDE_TRANSLATE_UNSUPPORTED(optional, const struct side_type_optional)
SIDE_TRANSLATE_UNSUPPORTED(enum_bitmap, const struct side_type_enum_bitmap)

/*
 * The gather types describe a value read from an address rather than
 * copied onto the argument vector, and carry the type of that value
 * within a gather wrapper. How the value is reached is the concern of
 * the libside visitor, which resolves the access mode and the offsets
 * and hands the tracer the value itself, so the translation describes
 * the wrapped type and nothing else.
 */
static
void side_translate_gather_bool_type(const struct side_type_gather_bool *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->fail)
		return;
	/* Reject bit-packed booleans, like the stack-copy flavour. */
	if (t->offset_bits) {
		side_translate_set_fail(ctx);
		return;
	}
	side_translate_bool_type(&t->type, priv);
}

static
void side_translate_gather_integer_type(const struct side_type_gather_integer *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->fail)
		return;
	/* Reject bit-packed integers, like the stack-copy flavour. */
	if (t->offset_bits) {
		side_translate_set_fail(ctx);
		return;
	}
	side_translate_integer_type(&t->type, priv);
}

static
void side_translate_gather_pointer_type(const struct side_type_gather_integer *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->fail)
		return;
	if (t->offset_bits) {
		side_translate_set_fail(ctx);
		return;
	}
	side_translate_pointer_type(&t->type, priv);
}

static
void side_translate_gather_byte_type(const struct side_type_gather_byte *t, void *priv)
{
	side_translate_byte_type(&t->type, priv);
}

static
void side_translate_gather_float_type(const struct side_type_gather_float *t, void *priv)
{
	side_translate_float_type(&t->type, priv);
}

static
void side_translate_gather_string_type(const struct side_type_gather_string *t, void *priv)
{
	side_translate_string_type(&t->type, priv);
}

static
void side_translate_before_gather_enum(const struct side_type_gather_enum *t __attribute__((unused)),
		void *priv)
{
	side_translate_before_enum(NULL, priv);
}

static
void side_translate_after_gather_enum(const struct side_type_gather_enum *t, void *priv)
{
	side_translate_after_enum_mappings(side_ptr_get(t->mappings), priv);
}

static
void side_translate_before_gather_struct(const struct side_type_gather_struct *t, void *priv)
{
	side_translate_before_struct(side_ptr_rel_get(t->type), priv);
}

static
void side_translate_after_gather_struct(const struct side_type_gather_struct *t, void *priv)
{
	side_translate_after_struct(side_ptr_rel_get(t->type), priv);
}

static
void side_translate_before_gather_array(const struct side_type_gather_array *t, void *priv)
{
	side_translate_before_array(&t->type, priv);
}

static
void side_translate_after_gather_array(const struct side_type_gather_array *t, void *priv)
{
	side_translate_after_array(&t->type, priv);
}

/*
 * The number of elements of a gathered sequence is read from memory,
 * so it can differ between the size pass and the write pass of the
 * serialization. What is recorded is the length of the size pass, and
 * the elements which follow it are truncated or filled to match, which
 * requires knowing where they end. The write pass reads that extent
 * from the entry which follows the length in the list of sizes the
 * size pass built, so an element which appends an entry of its own
 * between the two would leave the write pass reading the wrong one.
 *
 * The elements accepted are therefore the ones which record nothing of
 * their own there: the gather scalars, and a gathered structure or
 * array built out of them, to any depth. A gathered string and a
 * gathered sequence both do record one, and are refused; they are also
 * the only two whose own size is not a property of the description.
 */
static
bool side_gather_elem_is_fixed_size(const struct side_type *elem_type)
{
	switch (side_enum_get(elem_type->type)) {
	case SIDE_TYPE_GATHER_BOOL:		/* Fall-through. */
	case SIDE_TYPE_GATHER_BYTE:		/* Fall-through. */
	case SIDE_TYPE_GATHER_INTEGER:		/* Fall-through. */
	case SIDE_TYPE_GATHER_POINTER:		/* Fall-through. */
	case SIDE_TYPE_GATHER_FLOAT:
		return true;
	case SIDE_TYPE_GATHER_ENUM:
	{
		const struct side_type *container =
			side_ptr_rel_get(elem_type->u.side_gather.u.side_enum.elem_type);

		return side_gather_elem_is_fixed_size(container);
	}
	case SIDE_TYPE_GATHER_STRUCT:
	{
		const struct side_type_struct *side_struct =
			side_ptr_rel_get(elem_type->u.side_gather.u.side_struct.type);
		uint32_t i;

		for (i = 0; i < side_array_length(&side_struct->fields); i++) {
			const struct side_event_field *field =
				side_array_rel_at(&side_struct->fields, i);

			if (!side_gather_elem_is_fixed_size(&field->side_type))
				return false;
		}
		return true;
	}
	case SIDE_TYPE_GATHER_ARRAY:
	{
		const struct side_type *container =
			side_ptr_rel_get(elem_type->u.side_gather.u.side_array.type.elem_type);

		return side_gather_elem_is_fixed_size(container);
	}
	default:
		return false;
	}
}

static
void side_translate_before_gather_vla(const struct side_type_gather_vla *t, void *priv)
{
	struct side_translate_ctx *ctx = (struct side_translate_ctx *) priv;

	if (ctx->fail)
		return;
	if (!side_gather_elem_is_fixed_size(side_ptr_rel_get(t->type.elem_type))) {
		side_translate_set_fail(ctx);
		return;
	}
	side_translate_before_vla(&t->type, priv);
}

static
void side_translate_after_length_gather_vla(const struct side_type_gather_vla *t, void *priv)
{
	side_translate_after_length_vla(&t->type, priv);
}

static
void side_translate_after_element_gather_vla(const struct side_type_gather_vla *t, void *priv)
{
	side_translate_after_element_vla(&t->type, priv);
}

static
const struct side_description_visitor_callbacks side_translate_visitor_callbacks = {
	.before_field_func = side_translate_before_field,
	.after_field_func = side_translate_after_field,

	.null_type_func = side_translate_unsupported_null,
	.bool_type_func = side_translate_bool_type,
	.integer_type_func = side_translate_integer_type,
	.byte_type_func = side_translate_byte_type,
	.pointer_type_func = side_translate_pointer_type,
	.float_type_func = side_translate_float_type,
	.string_type_func = side_translate_string_type,

	.before_struct_type_func = side_translate_before_struct,
	.after_struct_type_func = side_translate_after_struct,
	.before_variant_type_func = side_translate_before_variant,
	.after_variant_selector_type_func = side_translate_after_variant_selector,
	.after_variant_type_func = side_translate_after_variant,
	.before_option_func = side_translate_before_option,
	.before_array_type_func = side_translate_before_array,
	.after_array_type_func = side_translate_after_array,
	.before_vla_type_func = side_translate_before_vla,
	.after_length_vla_type_func = side_translate_after_length_vla,
	.after_element_vla_type_func = side_translate_after_element_vla,
	.before_optional_type_func = side_translate_unsupported_optional,

	.before_enum_type_func = side_translate_before_enum,
	.after_enum_type_func = side_translate_after_enum,
	.before_enum_bitmap_type_func = side_translate_unsupported_enum_bitmap,

	.gather_bool_type_func = side_translate_gather_bool_type,
	.gather_byte_type_func = side_translate_gather_byte_type,
	.gather_integer_type_func = side_translate_gather_integer_type,
	.gather_pointer_type_func = side_translate_gather_pointer_type,
	.gather_float_type_func = side_translate_gather_float_type,
	.gather_string_type_func = side_translate_gather_string_type,

	.before_gather_struct_type_func = side_translate_before_gather_struct,
	.after_gather_struct_type_func = side_translate_after_gather_struct,
	.before_gather_array_type_func = side_translate_before_gather_array,
	.after_gather_array_type_func = side_translate_after_gather_array,
	.before_gather_vla_type_func = side_translate_before_gather_vla,
	.after_length_gather_vla_type_func = side_translate_after_length_gather_vla,
	.after_element_gather_vla_type_func = side_translate_after_element_gather_vla,

	.before_gather_enum_type_func = side_translate_before_gather_enum,
	.after_gather_enum_type_func = side_translate_after_gather_enum,

	.dynamic_type_func = side_translate_unsupported_dynamic,
};

static
void lttng_ust_side_event_destroy(struct lttng_ust_side_event *se)
{
	if (!se)
		return;
	side_translate_fields_destroy(
		(const struct lttng_ust_event_field **) se->fields,
		se->nr_fields);
	side_translate_attributes_destroy(se->parent.attributes);
	free(se);
}

/* Release what a failed translation left behind. */
static
void side_translate_ctx_destroy(struct side_translate_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i <= ctx->nesting && i < SIDE_TRANSLATE_MAX_NESTING; i++) {
		side_translate_fields_destroy(ctx->scopes[i].fields,
			ctx->scopes[i].nr_fields);
		ctx->scopes[i].fields = NULL;
		ctx->scopes[i].nr_fields = 0;
	}
	side_translate_type_destroy(ctx->elem_type);
	ctx->elem_type = NULL;
	side_translate_type_destroy(ctx->vla_length_type);
	ctx->vla_length_type = NULL;
}

static
struct lttng_ust_side_event *lttng_ust_side_event_create(struct side_event_state *sstate,
		struct side_event_description *sdesc)
{
	struct lttng_ust_side_event *se;
	struct side_translate_ctx ctx = {};
	struct side_description_visitor visitor;
	enum side_loglevel loglevel;

	if (sdesc->flags & SIDE_EVENT_FLAG_VARIADIC) {
		DBG("Skipping side event %s:%s: variadic events are not supported",
			side_ptr_rel_get(sdesc->provider_name),
			side_ptr_rel_get(sdesc->event_name));
		return NULL;
	}
	se = zmalloc(sizeof(struct lttng_ust_side_event));
	if (!se)
		return NULL;
	CDS_INIT_LIST_HEAD(&se->node);
	ctx.se = se;
	ctx.sdesc = sdesc;
	ctx.state = SIDE_TRANSLATE_TOPLEVEL;
	visitor.callbacks = &side_translate_visitor_callbacks;
	visitor.priv = &ctx;
	visit_event_description(&visitor, sdesc);
	if (ctx.fail || ctx.nesting) {
		DBG("Skipping side event %s:%s: unsupported field types",
			side_ptr_rel_get(sdesc->provider_name),
			side_ptr_rel_get(sdesc->event_name));
		goto error;
	}
	/* The event payload is the outermost field scope. */
	se->fields = ctx.scopes[0].fields;
	se->nr_fields = ctx.scopes[0].nr_fields;
	ctx.scopes[0].fields = NULL;
	ctx.scopes[0].nr_fields = 0;
	se->side_desc = sdesc;
	se->side_state = sstate;
	loglevel = side_enum_get(sdesc->loglevel);
	if (loglevel <= SIDE_LOGLEVEL_DEBUG)
		se->loglevel = side_loglevel_to_lttng[loglevel];
	else
		se->loglevel = side_loglevel_to_lttng[SIDE_LOGLEVEL_DEBUG];
	se->loglevel_ptr = &se->loglevel;
	se->model_emf_uri_ptr = NULL;

	se->tp_class.struct_size = sizeof(struct lttng_ust_tracepoint_class);
	se->tp_class.fields = se->fields;
	se->tp_class.nr_fields = se->nr_fields;
	se->tp_class.probe_callback = lttng_ust_side_probe_marker;
	se->tp_class.signature = "side";
	se->tp_class.probe_desc = &se->probe_desc;

	se->parent.struct_size = sizeof(struct lttng_ust_event_desc);
	se->parent.event_name = side_ptr_rel_get(sdesc->event_name);
	se->parent.probe_desc = &se->probe_desc;
	se->parent.tp_class = &se->tp_class;
	se->parent.loglevel = &se->loglevel_ptr;
	se->parent.model_emf_uri = &se->model_emf_uri_ptr;
	se->parent.attributes = side_translate_attributes(
		side_array_rel_elements(&sdesc->attributes),
		side_array_length(&sdesc->attributes), NULL);

	se->event_desc_array[0] = &se->parent;

	se->probe_desc.struct_size = sizeof(struct lttng_ust_probe_desc);
	se->probe_desc.provider_name = side_ptr_rel_get(sdesc->provider_name);
	se->probe_desc.event_desc = se->event_desc_array;
	se->probe_desc.nr_events = 1;
	se->probe_desc.major = LTTNG_UST_PROVIDER_MAJOR;
	se->probe_desc.minor = LTTNG_UST_PROVIDER_MINOR;
	return se;

error:
	side_translate_ctx_destroy(&ctx);
	lttng_ust_side_event_destroy(se);
	return NULL;
}

/*
 * Payload serialization into the ring buffer, driven by the arg-vec
 * type visitor, in two passes: size/alignment computation, then
 * writes between event_reserve and event_commit. Dynamic lengths
 * (strings, sequences) are computed in the size pass and reused by
 * the write pass so both passes agree even if the application
 * concurrently modifies the pointed-to data.
 */
#define SIDE_SERIALIZE_MAX_DYN_LEN	16

struct side_serialize_ctx {
	bool write_pass;
	bool fail;
	size_t len;		/* Size computed, then size reserved. */
	size_t written;		/* Write pass only. */
	size_t align;
	/*
	 * Offset within the sub-buffer at which the payload is
	 * recorded. Alignment is relative to the beginning of the
	 * sub-buffer, so it is applied to this offset plus the position
	 * within the payload. It is zero while the layout of the event
	 * is known statically, which is the case as long as no field is
	 * more aligned than the payload itself: the reservation aligns
	 * the payload on the alignment of the event description, so the
	 * two are then congruent.
	 */
	unsigned long base;
	/*
	 * Elements of the gathered sequence being serialized: where
	 * they begin, and where they end in the write pass, which is
	 * where the size pass ended them.
	 */
	size_t seq_start;
	size_t seq_end;
	bool seq_open;
	size_t dyn_len[SIDE_SERIALIZE_MAX_DYN_LEN];
	unsigned int dyn_idx;
	struct lttng_ust_ring_buffer_ctx *bufctx;
	struct lttng_ust_channel_buffer *chan;
	/* Input of the passes, for the size pass to be run again. */
	const struct side_event_description *desc;
	const struct side_arg_vec *side_arg_vec;
	void *caller_addr;
	/*
	 * Whether the layout is laid out against the offset the payload
	 * is recorded at, which a variant requires, rather than against
	 * the payload itself.
	 */
	bool dynamic_layout;
};

static
void side_serialize_record(struct side_serialize_ctx *c, const void *src,
		size_t size, size_t align)
{
	if (c->fail)
		return;
	if (!c->write_pass) {
		c->len += lttng_ust_ring_buffer_align(c->base + c->len, align);
		c->len += size;
		if (align > c->align)
			c->align = align;
	} else {
		size_t offset = c->written;

		/*
		 * A gather type reads its value from memory, which the
		 * application can change between the size pass and the
		 * write pass, so the length of a gathered string or of
		 * a gathered sequence is not necessarily the one the
		 * reservation was sized with. Never write past it.
		 */
		offset += lttng_ust_ring_buffer_align(c->base + offset, align);
		/*
		 * Drop the elements a gathered sequence grew by: the
		 * length recorded is the one of the size pass, so the
		 * elements which follow it must be exactly as many.
		 */
		if (c->seq_open && offset + size > c->seq_end)
			return;
		if (offset + size > c->len) {
			c->fail = true;
			return;
		}
		c->written = offset + size;
		c->chan->ops->event_write(c->bufctx, src, size, align);
	}
}

/* Skip to the next position with the given alignment. */
static
void side_serialize_align(struct side_serialize_ctx *c, size_t align)
{
	if (c->fail)
		return;
	if (!align) {
		c->fail = true;
		return;
	}
	if (!c->write_pass) {
		c->len += lttng_ust_ring_buffer_align(c->base + c->len, align);
		if (align > c->align)
			c->align = align;
	} else {
		size_t offset = c->written;

		/*
		 * The padding is part of what the size pass reserved,
		 * so the write pass accounts for it the same way it
		 * accounts for what it records.
		 */
		offset += lttng_ust_ring_buffer_align(c->base + offset, align);
		if (c->seq_open && offset > c->seq_end)
			return;
		if (offset > c->len) {
			c->fail = true;
			return;
		}
		c->written = offset;
		c->chan->ops->event_write(c->bufctx, "", 0, align);
	}
}

static
bool side_serialize_push_dyn(struct side_serialize_ctx *c, size_t len)
{
	if (c->dyn_idx >= SIDE_SERIALIZE_MAX_DYN_LEN) {
		c->fail = true;
		return false;
	}
	c->dyn_len[c->dyn_idx++] = len;
	return true;
}

static
size_t side_serialize_next_dyn(struct side_serialize_ctx *c)
{
	if (c->dyn_idx >= SIDE_SERIALIZE_MAX_DYN_LEN) {
		c->fail = true;
		return 0;
	}
	return c->dyn_len[c->dyn_idx++];
}

/*
 * The size of the region of the elements of a gathered sequence is
 * pushed after its length, and nothing is pushed in between: the
 * elements of such a sequence are of a fixed size, which the
 * translation enforces. The write pass therefore knows the region
 * before it serializes the elements, by looking at the entry which
 * follows the one it just took.
 */
static
size_t side_serialize_peek_dyn(struct side_serialize_ctx *c)
{
	if (c->dyn_idx >= SIDE_SERIALIZE_MAX_DYN_LEN) {
		c->fail = true;
		return 0;
	}
	return c->dyn_len[c->dyn_idx];
}

static
void side_serialize_integer_value(struct side_serialize_ctx *c,
		uint16_t integer_size, uint64_t v)
{
	size_t align = side_integer_alignof(integer_size);

	switch (integer_size) {
	case 1:
	{
		uint8_t tmp = (uint8_t) v;

		side_serialize_record(c, &tmp, sizeof(tmp), align);
		break;
	}
	case 2:
	{
		uint16_t tmp = (uint16_t) v;

		side_serialize_record(c, &tmp, sizeof(tmp), align);
		break;
	}
	case 4:
	{
		uint32_t tmp = (uint32_t) v;

		side_serialize_record(c, &tmp, sizeof(tmp), align);
		break;
	}
	case 8:
	{
		uint64_t tmp = v;

		side_serialize_record(c, &tmp, sizeof(tmp), align);
		break;
	}
	default:
		c->fail = true;
		break;
	}
}

/*
 * Fill what the write pass left of the reservation. A sequence whose
 * length is read from memory by a gather type can have fewer elements
 * in the write pass than the size pass reserved for, and every byte
 * reserved must be written: an unwritten byte would carry into the
 * trace whatever the ring buffer held there before.
 */
static
void side_serialize_pad_to(struct side_serialize_ctx *c, size_t end)
{
	static const char pad[16];

	if (!c->write_pass)
		return;
	if (end > c->len)
		end = c->len;
	while (c->written < end) {
		size_t len = end - c->written;

		if (len > sizeof(pad))
			len = sizeof(pad);
		c->written += len;
		c->chan->ops->event_write(c->bufctx, pad, len, 1);
	}
}

static
void side_serialize_pad(struct side_serialize_ctx *c)
{
	side_serialize_pad_to(c, c->len);
}

/*
 * The length of a sequence is counted by the tracer rather than
 * emitted by the application, so it is produced in the byte order of
 * the host and converted to the byte order its length type declares,
 * which is the one the description of the length field carries.
 */
static
void side_serialize_length_value(struct side_serialize_ctx *c,
		const struct side_type_integer *t, uint64_t len)
{
	if (side_type_reverse_byte_order(side_enum_get(t->byte_order))) {
		switch (t->integer_size) {
		case 1:
			break;
		case 2:
			len = side_bswap_16((uint16_t) len);
			break;
		case 4:
			len = side_bswap_32((uint32_t) len);
			break;
		case 8:
			len = side_bswap_64(len);
			break;
		default:
			c->fail = true;
			return;
		}
	}
	side_serialize_integer_value(c, t->integer_size, len);
}

static
void side_serialize_integer_from_value(struct side_serialize_ctx *c,
		const struct side_type_integer *t,
		const union side_integer_value *value)
{
	uint64_t v;

	if (c->fail)
		return;
	/*
	 * Record the value as it was emitted, in its own byte order:
	 * the description tells the reader of the trace which one that
	 * is. Load it unsigned, which preserves the bit pattern of a
	 * signed value as well.
	 */
	switch (t->integer_size) {
	case 1:
		v = value->side_u8;
		break;
	case 2:
		v = value->side_u16;
		break;
	case 4:
		v = value->side_u32;
		break;
	case 8:
		v = value->side_u64;
		break;
	default:
		c->fail = true;
		return;
	}
	side_serialize_integer_value(c, t->integer_size, v);
}

static
void side_serialize_integer(const struct side_type *type_desc,
		const struct side_arg *item, void *priv)
{
	side_serialize_integer_from_value((struct side_serialize_ctx *) priv,
		&type_desc->u.side_integer,
		&item->u.side_static.integer_value);
}

static
void side_serialize_bool_from_value(struct side_serialize_ctx *c,
		const struct side_type_bool *t,
		const union side_bool_value *value)
{
	uint64_t v;

	if (c->fail)
		return;
	switch (t->bool_size) {
	case 1:
		v = value->side_bool8;
		break;
	case 2:
		v = value->side_bool16;
		break;
	case 4:
		v = value->side_bool32;
		break;
	case 8:
		v = value->side_bool64;
		break;
	default:
		c->fail = true;
		return;
	}
	side_serialize_integer_value(c, t->bool_size, v);
}

static
void side_serialize_bool(const struct side_type *type_desc,
		const struct side_arg *item, void *priv)
{
	side_serialize_bool_from_value((struct side_serialize_ctx *) priv,
		&type_desc->u.side_bool, &item->u.side_static.bool_value);
}

static
void side_serialize_byte(const struct side_type *type_desc __attribute__((unused)),
		const struct side_arg *item, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;

	side_serialize_record(c, &item->u.side_static.byte_value,
		sizeof(uint8_t), lttng_ust_rb_alignof(uint8_t));
}

static
void side_serialize_float_from_value(struct side_serialize_ctx *c,
		const struct side_type_float *t,
		const union side_float_value *value)
{
	if (c->fail)
		return;
	switch (t->float_size) {
	case 4:
	{
		float f = value->side_float_binary32;

		side_serialize_record(c, &f, sizeof(f), lttng_ust_rb_alignof(float));
		break;
	}
	case 8:
	{
		double d = value->side_float_binary64;

		side_serialize_record(c, &d, sizeof(d), lttng_ust_rb_alignof(double));
		break;
	}
	default:
		c->fail = true;
		break;
	}
}

static
void side_serialize_float(const struct side_type *type_desc,
		const struct side_arg *item, void *priv)
{
	side_serialize_float_from_value((struct side_serialize_ctx *) priv,
		&type_desc->u.side_float, &item->u.side_static.float_value);
}

/*
 * A string is recorded with its terminator, and its length is the one
 * of the size pass, which is the one the reservation was made with.
 *
 * A string read from memory by a gather type, and a string whose
 * argument the application mutates, can be of a different length in
 * the write pass. The ring buffer resolves that the same way it does
 * for a tracepoint: the copy stops at the terminator or at the length,
 * pads with '#' if the string became shorter, and always terminates.
 * The field is therefore always exactly as long as reserved, and
 * always well formed.
 */
static
void side_serialize_string_from_ptr(struct side_serialize_ctx *c, const char *p)
{
	size_t len;

	if (c->fail)
		return;
	if (!p)
		p = "";
	if (!c->write_pass) {
		len = strlen(p) + 1;
		if (!side_serialize_push_dyn(c, len))
			return;
		c->len += len;		/* Strings are byte aligned. */
		return;
	}
	len = side_serialize_next_dyn(c);
	if (c->fail)
		return;
	if (c->written + len > c->len) {
		c->fail = true;
		return;
	}
	c->written += len;
	c->chan->ops->event_strcpy(c->bufctx, p, len);
}

static
void side_serialize_string(const struct side_type *type_desc,
		const struct side_arg *item, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;
	const struct side_type_string *t = &type_desc->u.side_string;
	const char *p;

	if (c->fail)
		return;
	if (t->unit_size != 1) {
		c->fail = true;
		return;
	}
	p = (const char *) side_ptr_get(item->u.side_static.string_value);
	side_serialize_string_from_ptr(c, p);
}

/*
 * The argument of an enumeration field holds the value of its
 * container, which is what is written into the ring buffer.
 */
static
void side_serialize_enum(const struct side_type *type_desc,
		const struct side_arg *item, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;
	const struct side_type *container;

	if (c->fail)
		return;
	container = side_ptr_rel_get(type_desc->u.side_enum.elem_type);
	switch (side_enum_get(container->type)) {
	case SIDE_TYPE_U8:		/* Fall-through. */
	case SIDE_TYPE_U16:		/* Fall-through. */
	case SIDE_TYPE_U32:		/* Fall-through. */
	case SIDE_TYPE_U64:		/* Fall-through. */
	case SIDE_TYPE_S8:		/* Fall-through. */
	case SIDE_TYPE_S16:		/* Fall-through. */
	case SIDE_TYPE_S32:		/* Fall-through. */
	case SIDE_TYPE_S64:
		break;
	default:
		c->fail = true;
		return;
	}
	side_serialize_integer(container, item, priv);
}

/*
 * The selector of a side variant travels with its argument: write it
 * into the hidden field which precedes the variant, which is what the
 * trace describes the variant by. The selected option is serialized
 * next, by the visitor.
 */
static
void side_serialize_before_variant(const struct side_type_variant *side_type_variant,
		const struct side_arg_variant *side_arg_variant, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;
	const struct side_type *selector_type = &side_type_variant->selector;

	if (c->fail)
		return;
	switch (side_enum_get(selector_type->type)) {
	case SIDE_TYPE_U8:		/* Fall-through. */
	case SIDE_TYPE_U16:		/* Fall-through. */
	case SIDE_TYPE_U32:		/* Fall-through. */
	case SIDE_TYPE_U64:		/* Fall-through. */
	case SIDE_TYPE_S8:		/* Fall-through. */
	case SIDE_TYPE_S16:		/* Fall-through. */
	case SIDE_TYPE_S32:		/* Fall-through. */
	case SIDE_TYPE_S64:
		break;
	default:
		c->fail = true;
		return;
	}
	side_serialize_integer(selector_type, &side_arg_variant->selector, priv);
}

static
void side_serialize_before_struct(const struct side_type_struct *side_struct,
		const struct side_arg_vec *side_arg_vec, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;

	if (c->fail)
		return;
	if (side_arg_vec->len != side_array_length(&side_struct->fields)) {
		c->fail = true;
		return;
	}
	/*
	 * A structure is aligned on the largest alignment of the fields
	 * it contains: this is the alignment a trace reader applies,
	 * since the metadata does not state an explicit alignment.
	 */
	side_serialize_align(c, side_struct_alignof(side_struct));
	/* Members are serialized through the field callbacks. */
}

/*
 * A reader aligns an array or a sequence on the alignment of its
 * elements before it reads them, whether there are any or not, so the
 * tracer applies it here rather than leaving it to the first element.
 */
static
void side_serialize_align_elements(struct side_serialize_ctx *c,
		const struct side_type *elem_type)
{
	size_t align = side_type_alignof(elem_type);

	if (!align) {
		c->fail = true;
		return;
	}
	side_serialize_align(c, align);
}

static
void side_serialize_before_array(const struct side_type_array *side_array,
		const struct side_arg_vec *side_arg_vec, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;

	if (c->fail)
		return;
	if (side_arg_vec->len != side_array->length) {
		c->fail = true;
		return;
	}
	side_serialize_align_elements(c, side_ptr_rel_get(side_array->elem_type));
	/* Elements are serialized through the element callbacks. */
}

static
void side_serialize_before_vla(const struct side_type_vla *side_vla,
		const struct side_arg_vec *side_arg_vec, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;
	const struct side_type *lt;
	const struct side_type_integer *t;
	size_t len;

	if (c->fail)
		return;
	lt = side_ptr_rel_get(side_vla->length_type);
	switch (side_enum_get(lt->type)) {
	case SIDE_TYPE_U8:
	case SIDE_TYPE_U16:
	case SIDE_TYPE_U32:
	case SIDE_TYPE_U64:
		break;
	default:
		c->fail = true;
		return;
	}
	t = &lt->u.side_integer;
	if (!c->write_pass) {
		len = side_arg_vec->len;
		if (t->integer_size < 8
				&& len >= (1ULL << (t->integer_size * CHAR_BIT))) {
			c->fail = true;
			return;
		}
		if (!side_serialize_push_dyn(c, len))
			return;
	} else {
		len = side_serialize_next_dyn(c);
		if (c->fail)
			return;
	}
	side_serialize_length_value(c, t, len);
	side_serialize_align_elements(c, side_ptr_rel_get(side_vla->elem_type));
	/* Elements are serialized through the element callbacks. */
}

static
void side_serialize_fail_variadic(const struct side_arg_dynamic_struct *var_struct __attribute__((unused)),
		void *priv)
{
	((struct side_serialize_ctx *) priv)->fail = true;
}

static
void side_serialize_fail_arg(const struct side_type *type_desc __attribute__((unused)),
		const struct side_arg *item __attribute__((unused)), void *priv)
{
	((struct side_serialize_ctx *) priv)->fail = true;
}

static
void side_serialize_fail_dynamic(const struct side_arg *item __attribute__((unused)),
		void *priv)
{
	((struct side_serialize_ctx *) priv)->fail = true;
}

/*
 * The gather types read their value from an address instead of taking
 * it from the argument vector. The libside visitor resolves the access
 * mode and the offsets and hands the tracer the value itself, so what
 * is recorded, and how, is the same as for the stack-copy flavour.
 */
static
void side_serialize_gather_bool(const struct side_type_gather_bool *t,
		const union side_bool_value *value, void *priv)
{
	side_serialize_bool_from_value((struct side_serialize_ctx *) priv,
		&t->type, value);
}

static
void side_serialize_gather_integer(const struct side_type_gather_integer *t,
		const union side_integer_value *value, void *priv)
{
	side_serialize_integer_from_value((struct side_serialize_ctx *) priv,
		&t->type, value);
}

static
void side_serialize_gather_byte(const struct side_type_gather_byte *t __attribute__((unused)),
		const uint8_t *p, void *priv)
{
	side_serialize_record((struct side_serialize_ctx *) priv, p,
		sizeof(uint8_t), lttng_ust_rb_alignof(uint8_t));
}

static
void side_serialize_gather_float(const struct side_type_gather_float *t,
		const union side_float_value *value, void *priv)
{
	side_serialize_float_from_value((struct side_serialize_ctx *) priv,
		&t->type, value);
}

static
void side_serialize_gather_string(const struct side_type_gather_string *t __attribute__((unused)),
		const void *p, uint8_t unit_size,
		enum side_type_label_byte_order byte_order __attribute__((unused)),
		size_t strlen_with_null __attribute__((unused)), void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;

	if (c->fail)
		return;
	if (unit_size != 1) {
		c->fail = true;
		return;
	}
	side_serialize_string_from_ptr(c, (const char *) p);
}

static
void side_serialize_gather_enum(const struct side_type_gather_enum *t,
		const union side_integer_value *value, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;
	const struct side_type *container;

	if (c->fail)
		return;
	/* The container of a gathered enumeration is a gathered integer. */
	container = side_ptr_rel_get(t->elem_type);
	if (side_enum_get(container->type) != SIDE_TYPE_GATHER_INTEGER) {
		c->fail = true;
		return;
	}
	side_serialize_integer_from_value(c,
		&container->u.side_gather.u.side_integer.type, value);
}

static
void side_serialize_before_gather_struct(const struct side_type_struct *side_struct,
		void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;

	if (c->fail)
		return;
	/* Aligned like a stack-copy structure. */
	side_serialize_align(c, side_struct_alignof(side_struct));
	/* Members are serialized through the field callbacks. */
}

/*
 * The elements of a gathered sequence are emitted by the libside
 * visitor according to the length it reads on each pass, which the
 * application can change in between. The length recorded is the one of
 * the size pass, which is the one the reservation was made with, and
 * the write budget of side_serialize_record() keeps a length which
 * grew from writing past that reservation.
 */
static
void side_serialize_before_gather_array(const struct side_type_array *side_array,
		void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;

	if (c->fail)
		return;
	side_serialize_align_elements(c, side_ptr_rel_get(side_array->elem_type));
	/* Elements are serialized through the element callbacks. */
}

static
void side_serialize_before_gather_vla(const struct side_type_vla *side_vla,
		uint32_t length, void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;
	const struct side_type_integer *t;
	const struct side_type *lt;
	size_t len;

	if (c->fail)
		return;
	if (c->seq_open) {
		/* Sequences of sequences are refused by the translation. */
		c->fail = true;
		return;
	}
	lt = side_ptr_rel_get(side_vla->length_type);
	/* The length of a gathered sequence is gathered as well. */
	if (side_enum_get(lt->type) != SIDE_TYPE_GATHER_INTEGER) {
		c->fail = true;
		return;
	}
	t = &lt->u.side_gather.u.side_integer.type;
	if (t->signedness) {
		c->fail = true;
		return;
	}
	if (!c->write_pass) {
		len = length;
		if (t->integer_size < 8
				&& len >= (1ULL << (t->integer_size * CHAR_BIT))) {
			c->fail = true;
			return;
		}
		if (!side_serialize_push_dyn(c, len))
			return;
	} else {
		len = side_serialize_next_dyn(c);
		if (c->fail)
			return;
	}
	side_serialize_length_value(c, t, len);
	side_serialize_align_elements(c, side_ptr_rel_get(side_vla->elem_type));
	if (c->fail)
		return;
	/* Elements are serialized through the element callbacks. */
	c->seq_open = true;
	if (!c->write_pass) {
		c->seq_start = c->len;
		c->seq_end = SIZE_MAX;
	} else {
		c->seq_start = c->written;
		c->seq_end = c->written + side_serialize_peek_dyn(c);
	}
}

static
void side_serialize_after_gather_vla(const struct side_type_vla *side_vla __attribute__((unused)),
		uint32_t length __attribute__((unused)), void *priv)
{
	struct side_serialize_ctx *c = (struct side_serialize_ctx *) priv;
	size_t size;

	if (!c->seq_open)
		return;
	c->seq_open = false;
	if (c->fail)
		return;
	if (!c->write_pass) {
		/*
		 * The elements of a gathered sequence occupy what they
		 * did in the size pass, whatever the length read from
		 * memory has become since.
		 */
		(void) side_serialize_push_dyn(c, c->len - c->seq_start);
		return;
	}
	size = side_serialize_next_dyn(c);
	if (c->fail)
		return;
	/* Fill for the elements the sequence shrank by. */
	side_serialize_pad_to(c, c->seq_start + size);
}

const struct side_type_visitor side_serialize_type_visitor = {
	.before_variadic_fields_func = side_serialize_fail_variadic,

	.null_type_func = side_serialize_fail_arg,
	.bool_type_func = side_serialize_bool,
	.integer_type_func = side_serialize_integer,
	.byte_type_func = side_serialize_byte,
	.pointer_type_func = side_serialize_integer,
	.float_type_func = side_serialize_float,
	.string_type_func = side_serialize_string,

	.before_struct_type_func = side_serialize_before_struct,
	.before_variant_type_func = side_serialize_before_variant,
	.before_array_type_func = side_serialize_before_array,
	.before_vla_type_func = side_serialize_before_vla,

	.enum_type_func = side_serialize_enum,
	.enum_bitmap_type_func = side_serialize_fail_arg,

	.gather_bool_type_func = side_serialize_gather_bool,
	.gather_byte_type_func = side_serialize_gather_byte,
	.gather_integer_type_func = side_serialize_gather_integer,
	.gather_pointer_type_func = side_serialize_gather_integer,
	.gather_float_type_func = side_serialize_gather_float,
	.gather_string_type_func = side_serialize_gather_string,

	.before_gather_struct_type_func = side_serialize_before_gather_struct,
	.before_gather_array_type_func = side_serialize_before_gather_array,
	.before_gather_vla_type_func = side_serialize_before_gather_vla,
	.after_gather_vla_type_func = side_serialize_after_gather_vla,

	.gather_enum_type_func = side_serialize_gather_enum,

	.dynamic_null_func = side_serialize_fail_dynamic,
	.dynamic_bool_func = side_serialize_fail_dynamic,
	.dynamic_integer_func = side_serialize_fail_dynamic,
	.dynamic_byte_func = side_serialize_fail_dynamic,
	.dynamic_pointer_func = side_serialize_fail_dynamic,
	.dynamic_float_func = side_serialize_fail_dynamic,
	.dynamic_string_func = side_serialize_fail_dynamic,
};

/*
 * Whether a field of this event is aligned against the offset it is
 * recorded at rather than against the payload. A variant is: CTF 2
 * gives it an alignment of one, so it does not raise the alignment of
 * the payload, while its selected option keeps the alignment of its
 * own type.
 */
static
bool side_type_layout_is_dynamic(const struct side_type *type_desc)
{
	switch (side_enum_get(type_desc->type)) {
	case SIDE_TYPE_VARIANT:
		return true;
	case SIDE_TYPE_STRUCT:
	{
		const struct side_type_struct *side_struct =
			side_ptr_rel_get(type_desc->u.side_struct);
		uint32_t i, nr_fields = side_array_length(&side_struct->fields);

		for (i = 0; i < nr_fields; i++) {
			const struct side_event_field *f =
				side_array_rel_at(&side_struct->fields, i);

			if (side_type_layout_is_dynamic(&f->side_type))
				return true;
		}
		return false;
	}
	case SIDE_TYPE_ARRAY:
		return side_type_layout_is_dynamic(side_ptr_rel_get(
			side_ptr_rel_get(type_desc->u.side_array)->elem_type));
	case SIDE_TYPE_VLA:
		return side_type_layout_is_dynamic(side_ptr_rel_get(
			side_ptr_rel_get(type_desc->u.side_vla)->elem_type));
	default:
		return false;
	}
}

static
bool side_event_layout_is_dynamic(const struct side_event_description *desc)
{
	uint32_t i, nr_fields = side_array_length(&desc->fields);

	for (i = 0; i < nr_fields; i++) {
		const struct side_event_field *f = side_array_rel_at(&desc->fields, i);

		if (side_type_layout_is_dynamic(&f->side_type))
			return true;
	}
	return false;
}

/*
 * Lay the payload out at @base, the offset within the sub-buffer it is
 * recorded at, and answer its size. Returns -1 if it cannot be laid
 * out. Every state the pass produces is reset here: it runs once per
 * reservation attempt, and the attempt which takes the reservation is
 * the last one to have run.
 */
static
ssize_t side_serialize_size_pass(struct side_serialize_ctx *c, unsigned long base)
{
	c->write_pass = false;
	c->fail = false;
	c->len = 0;
	c->written = 0;
	c->dyn_idx = 0;
	c->seq_open = false;
	c->base = base;
	c->align = side_event_alignof(c->desc);
	type_visitor_event(&side_serialize_type_visitor, c->desc,
		c->side_arg_vec, NULL, c->caller_addr, c);
	if (caa_unlikely(c->fail))
		return -1;
	/*
	 * The payload is aligned on the alignment of the event
	 * description, which is what the reader of the trace derives.
	 * A statically laid out event which records a field more
	 * aligned than that would place it where the reader does not
	 * expect it, so refuse it rather than write it: the alignment
	 * of one of its types is missing from side_type_alignof(). An
	 * event laid out against the offset it is recorded at may
	 * record a field more aligned than its payload, which is what a
	 * variant option does.
	 */
	if (caa_unlikely(!c->dynamic_layout && c->align != side_event_alignof(c->desc))) {
		DBG("Side event %s:%s records a field more aligned than its description",
			side_ptr_rel_get(c->desc->provider_name),
			side_ptr_rel_get(c->desc->event_name));
		return -1;
	}
	return (ssize_t) c->len;
}

/* Size callback of the reservation, for a dynamic layout. */
static
ssize_t side_serialize_get_data_size(void *priv, unsigned long payload_offset)
{
	return side_serialize_size_pass((struct side_serialize_ctx *) priv,
					payload_offset);
}

/*
 * Called by side with the side RCU read-side held: the LTTng event
 * (priv) is protected against teardown by the synchronous callback
 * unregistration grace period in unregister_event().
 */
static
void tracer_call(const struct side_event_description *desc,
		const struct side_arg_vec *side_arg_vec,
		void *priv, void *caller_addr)
{
	struct lttng_ust_event_common *event = (struct lttng_ust_event_common *) priv;
	struct lttng_ust_channel_common *chan_common;
	struct lttng_ust_probe_ctx probe_ctx;

	if (caa_unlikely(!CMM_ACCESS_ONCE(event->enabled)))
		return;
	chan_common = lttng_ust_get_chan_common_from_event_common(event);
	if (chan_common) {
		if (caa_unlikely(!CMM_ACCESS_ONCE(chan_common->session->active)))
			return;
		if (caa_unlikely(!CMM_ACCESS_ONCE(chan_common->enabled)))
			return;
	}
	probe_ctx.struct_size = sizeof(struct lttng_ust_probe_ctx);
	probe_ctx.ip = caller_addr;
	if (caa_unlikely(CMM_ACCESS_ONCE(event->eval_filter))) {
		if (caa_unlikely(event->run_filter(event,
				(const char *) side_arg_vec, &probe_ctx, NULL)
					!= LTTNG_UST_EVENT_FILTER_ACCEPT))
			return;
	}
	switch (event->type) {
	case LTTNG_UST_EVENT_TYPE_RECORDER:
	{
		struct lttng_ust_event_recorder *event_recorder =
			(struct lttng_ust_event_recorder *) event->child;
		struct lttng_ust_channel_buffer *chan = event_recorder->chan;
		struct lttng_ust_ring_buffer_ctx bufctx;
		/*
		 * The serialization context is built where it is used,
		 * which is only here: it is the largest thing this
		 * function has, and its unnamed members are zeroed by
		 * this initializer. Building it on entry would put that
		 * on the path of an event which is disabled, of one a
		 * filter rejects, and of a notifier or a counter, none
		 * of which serialize anything.
		 */
		struct side_serialize_ctx c = {
			.desc = desc,
			.side_arg_vec = side_arg_vec,
			.caller_addr = caller_addr,
		};

		c.chan = chan;
		c.align = side_event_alignof(desc);
		c.dynamic_layout = side_event_layout_is_dynamic(desc);
		if (c.dynamic_layout) {
			/*
			 * A field of this event is aligned against the
			 * offset it is recorded at rather than against
			 * the payload, so its size is only known once
			 * the reservation has that offset. The
			 * reservation computes it, and computes it
			 * again if it has to be retried.
			 */
			if (caa_unlikely(!chan->ops->event_reserve_dyn)) {
				DBG("Side event %s:%s needs a reservation which computes the size of its payload, which this channel does not provide",
					side_ptr_rel_get(desc->provider_name),
					side_ptr_rel_get(desc->event_name));
				return;
			}
			lttng_ust_ring_buffer_ctx_init(&bufctx, event_recorder,
				0, c.align, &probe_ctx);
			if (chan->ops->event_reserve_dyn(&bufctx,
					side_serialize_get_data_size, &c) < 0)
				return;
		} else {
			/* Size/alignment pass. */
			if (side_serialize_size_pass(&c, 0) < 0)
				return;
			lttng_ust_ring_buffer_ctx_init(&bufctx, event_recorder,
				c.len, c.align, &probe_ctx);
			if (chan->ops->event_reserve(&bufctx) < 0)
				return;
		}
		/* Write pass. c.len is now the size reserved. */
		c.write_pass = true;
		c.written = 0;
		c.dyn_idx = 0;
		c.bufctx = &bufctx;
		type_visitor_event(&side_serialize_type_visitor, desc,
			side_arg_vec, NULL, caller_addr, &c);
		side_serialize_pad(&c);
		chan->ops->event_commit(&bufctx);
		break;
	}
	case LTTNG_UST_EVENT_TYPE_NOTIFIER:
	{
		struct lttng_ust_event_notifier *event_notifier =
			(struct lttng_ust_event_notifier *) event->child;
		struct lttng_ust_notification_ctx notif_ctx;

		notif_ctx.struct_size = sizeof(struct lttng_ust_notification_ctx);
		notif_ctx.eval_capture = CMM_ACCESS_ONCE(event_notifier->eval_capture);
		/*
		 * Unlike tracepoints, there is no interpreter stack to
		 * prepare for the capture bytecode: the side argument
		 * vector is the interpreter input.
		 */
		event_notifier->notification_send(event_notifier,
			(const char *) side_arg_vec, &probe_ctx, &notif_ctx);
		break;
	}
	case LTTNG_UST_EVENT_TYPE_COUNTER:
	{
		struct lttng_ust_event_counter *event_counter =
			(struct lttng_ust_event_counter *) event->child;
		struct lttng_ust_event_counter_ctx event_counter_ctx;

		event_counter_ctx.struct_size = sizeof(struct lttng_ust_event_counter_ctx);
		event_counter_ctx.args_available = CMM_ACCESS_ONCE(event_counter->use_args);
		(void) event_counter->chan->ops->counter_hit(event_counter,
			(const char *) side_arg_vec, &probe_ctx,
			&event_counter_ctx);
		break;
	}
	}
}

/*
 * Recognition and connection of side-backed event descriptors,
 * called from register_event()/unregister_event() under ust_mutex.
 */
bool lttng_ust_side_is_side_event(const struct lttng_ust_event_desc *desc)
{
	return desc->tp_class
		&& desc->tp_class->probe_callback == lttng_ust_side_probe_marker;
}

const struct side_event_description *lttng_ust_side_get_side_desc(
		const struct lttng_ust_event_desc *desc)
{
	const struct lttng_ust_side_event *se;

	if (!lttng_ust_side_is_side_event(desc))
		return NULL;
	se = caa_container_of(desc, const struct lttng_ust_side_event, parent);
	return se->side_desc;
}

/*
 * Key identifying the side event callbacks of an event: the key of the
 * session which owns it, so that a statedump requested by a session is
 * only delivered to that session. Events which do not belong to a
 * session, the event notifiers, use the key of the tracer: they never
 * request a statedump, and never receive one.
 */
static
uint64_t side_event_key(struct lttng_ust_event_common *event)
{
	struct lttng_ust_channel_common *chan_common;

	chan_common = lttng_ust_get_chan_common_from_event_common(event);
	if (!chan_common || !chan_common->session->priv->side_key)
		return tracer_key;
	return chan_common->session->priv->side_key;
}

/*
 * Number of deferred registrations and unregistrations performed since
 * the last reclaim: the memory they leave behind is held until then,
 * so reclaim within a batch which grows past this bound rather than
 * letting an arbitrarily long batch, e.g. the synchronization of the
 * enablers of a session which has many events, hold it all.
 *
 * The count is an upper bound of the number of queued callback arrays:
 * a registration which replaces the empty callback array, or an
 * unregistration which leaves it behind, has nothing to queue.
 *
 * Protected by ust_mutex, like the registration and unregistration of
 * the events themselves.
 */
#define SIDE_RELEASE_QUEUE_BATCH_LEN	32

static unsigned long side_release_queue_len;

static
void side_release_queue_account(void)
{
	if (++side_release_queue_len >= SIDE_RELEASE_QUEUE_BATCH_LEN)
		lttng_ust_side_prune_release_queue();
}

int lttng_ust_side_register_event(const struct lttng_ust_event_desc *desc,
		struct lttng_ust_event_common *event)
{
	struct lttng_ust_side_event *se =
		caa_container_of(desc, struct lttng_ust_side_event, parent);

	if (side_tracer_callback_register_defer(se->side_state, tracer_call,
			event, side_event_key(event)) != SIDE_ERROR_OK)
		return -EINVAL;
	side_release_queue_account();
	return 0;
}

int lttng_ust_side_unregister_event(const struct lttng_ust_event_desc *desc,
		struct lttng_ust_event_common *event)
{
	struct lttng_ust_side_event *se =
		caa_container_of(desc, struct lttng_ust_side_event, parent);

	if (side_tracer_callback_unregister_defer(se->side_state, tracer_call,
			event, side_event_key(event)) != SIDE_ERROR_OK)
		return -EINVAL;
	side_release_queue_account();
	return 0;
}

/*
 * Reclaim the memory left behind by the deferred registration and
 * unregistration of the side event callbacks. A single grace period of
 * the side event domain is issued for the whole batch, rather than one
 * per event.
 */
void lttng_ust_side_prune_release_queue(void)
{
	if (!side_release_queue_len)
		return;
	side_release_queue_len = 0;
	side_tracer_callback_synchronize();
	side_tracer_callback_reclaim();
}

static
void tracer_event_notification(enum side_tracer_notification notif,
		struct side_event_state **states, uint32_t nr_events,
		void *priv __attribute__((unused)))
{
	uint32_t i;

	switch (notif) {
	case SIDE_TRACER_NOTIFICATION_INSERT_EVENTS:
	{
		struct lttng_ust_side_registration *reg;

		reg = zmalloc(sizeof(struct lttng_ust_side_registration));
		if (!reg) {
			ERR("Error allocating side event registration");
			return;
		}
		reg->side_events = states;
		reg->nr_side_events = nr_events;
		CDS_INIT_LIST_HEAD(&reg->events);
		cds_list_add_tail(&reg->node, &side_registration_list);
		for (i = 0; i < nr_events; i++) {
			struct side_event_state *sstate = states[i];
			struct side_event_description *sdesc;
			struct lttng_ust_side_event *se;

			if (!sstate)
				continue;
			/*
			 * A notification hands over the state, and the
			 * description hangs off it: a description holds
			 * no address of its own, so the edge between the
			 * two runs this way.
			 */
			sdesc = side_event_state_description(sstate);
			if (!sdesc) {
				ERR("Side event state ABI version (%u) does not match the version supported by the tracer (%u)",
					sstate->version,
					SIDE_EVENT_STATE_ABI_VERSION);
				continue;
			}
			if (sdesc->version != SIDE_EVENT_DESCRIPTION_ABI_VERSION) {
				ERR("Side event description ABI version (%u) does not match the version supported by the tracer (%u)",
					sdesc->version,
					SIDE_EVENT_DESCRIPTION_ABI_VERSION);
				continue;
			}
			se = lttng_ust_side_event_create(sstate, sdesc);
			if (!se)
				continue;
			se->reg_probe = lttng_ust_probe_register(&se->probe_desc);
			if (!se->reg_probe) {
				ERR("Error registering probe provider for side event %s:%s",
					side_ptr_rel_get(sdesc->provider_name),
					side_ptr_rel_get(sdesc->event_name));
				lttng_ust_side_event_destroy(se);
				continue;
			}
			cds_list_add_tail(&se->node, &reg->events);
			DBG("Registered side event %s:%s with the LTTng tracer",
				side_ptr_rel_get(sdesc->provider_name),
				side_ptr_rel_get(sdesc->event_name));
		}
		break;
	}
	case SIDE_TRACER_NOTIFICATION_REMOVE_EVENTS:
	{
		struct lttng_ust_side_registration *reg;
		struct lttng_ust_side_event *se, *tmp;
		bool found = false;

		cds_list_for_each_entry(reg, &side_registration_list, node) {
			if (reg->side_events == states) {
				found = true;
				break;
			}
		}
		if (!found)
			return;
		cds_list_for_each_entry_safe(se, tmp, &reg->events, node) {
			/*
			 * Synchronously tears down every LTTng event
			 * referencing this descriptor (which
			 * unregisters the side callbacks through
			 * unregister_event) before the instrumented
			 * object may be unloaded.
			 */
			lttng_ust_probe_unregister(se->reg_probe);
			cds_list_del(&se->node);
			lttng_ust_side_event_destroy(se);
		}
		cds_list_del(&reg->node);
		free(reg);
		break;
	}
	}
}

void lttng_ust_side_tracer_init(void)
{
	if (side_tracer_request_key(&tracer_key))
		abort();
	tracer_handle = side_tracer_event_notification_register(tracer_event_notification, NULL);
	if (!tracer_handle)
		abort();
}

void lttng_ust_side_tracer_exit(void)
{
	side_tracer_event_notification_unregister(tracer_handle);
}

/*
 * The tracepoint probes are called from a LTTng-UST RCU read-side
 * critical section (lttng_ust_tp_rcu_read_lock()), and the side event
 * callbacks are called from a side read-side critical section, which
 * is a distinct domain. Data reachable from the probes is therefore
 * only quiescent once both domains have observed a grace period.
 */
/*
 * A session requests the state of the application: side dispatches the
 * request to the statedump callbacks registered by the application,
 * which emit their events with the key of the session, so that only
 * this session records them.
 */
int lttng_ust_side_session_key_alloc(struct lttng_ust_session *session)
{
	uint64_t key;

	if (session->priv->side_key)
		return 0;
	if (side_tracer_request_key(&key) != SIDE_ERROR_OK) {
		DBG("Unable to allocate a side key for the session");
		return -ENOMEM;
	}
	session->priv->side_key = key;
	return 0;
}

void lttng_ust_side_session_statedump(struct lttng_ust_session *session)
{
	if (!session->priv->side_key)
		return;
	if (side_tracer_statedump_request(session->priv->side_key) != SIDE_ERROR_OK)
		DBG("Unable to request a side statedump for the session");
}

void lttng_ust_tracer_synchronize(void)
{
	lttng_ust_urcu_synchronize_rcu();
	side_tracer_callback_synchronize();
}
