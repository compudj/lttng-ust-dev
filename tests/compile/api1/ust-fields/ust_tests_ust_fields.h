/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2014 Geneviève Bastien <gbastien@versatic.net>
 */

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER ust_tests_ust_fields

#if !defined(_TRACEPOINT_UST_TESTS_UST_FIELDS_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _TRACEPOINT_UST_TESTS_UST_FIELDS_H

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_ENUM(ust_tests_ust_fields, testenum,
	LTTNG_UST_TP_ENUM_VALUES(
		lttng_ust_field_enum_value("even", 0)
		lttng_ust_field_enum_value("uneven", 1)
		lttng_ust_field_enum_range("twoto4", 2, 4)
		lttng_ust_field_enum_value("five\"extra\\test", 5)
	)
)

LTTNG_UST_TRACEPOINT_ENUM(ust_tests_ust_fields, testenum2,
	LTTNG_UST_TP_ENUM_VALUES(
		lttng_ust_field_enum_value("zero", 0)
		lttng_ust_field_enum_value("five", 5)
		lttng_ust_field_enum_range("ten_to_twenty", 10, 20)
	)
)

/*
 * Enumeration field is used twice to make sure the type declaration
 * is entered only once in the metadata file.
 */
LTTNG_UST_TRACEPOINT_EVENT(ust_tests_ust_fields, tptest,
	LTTNG_UST_TP_ARGS(int, anint, int, enumval, int, enumval2),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(int, intfield, anint)
		lttng_ust_field_enum(ust_tests_ust_fields, testenum, int, enumfield, enumval)
		lttng_ust_field_enum(ust_tests_ust_fields, testenum, long long,
				enumfield_bis, enumval)
		lttng_ust_field_enum(ust_tests_ust_fields, testenum2, unsigned int,
				enumfield_third, enumval2)
	)
)

/*
 * Another tracepoint using the types to make sure each type is entered
 * only once in the metadata file.
 */
LTTNG_UST_TRACEPOINT_EVENT(ust_tests_ust_fields, tptest_bis,
	LTTNG_UST_TP_ARGS(int, anint, int, enumval),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(int, intfield, anint)
		lttng_ust_field_enum(ust_tests_ust_fields, testenum, unsigned char,
			enumfield, enumval)
	)
)

#endif /* _TRACEPOINT_UST_TESTS_UST_FIELDS_H */

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./ust_tests_ust_fields.h"

/* This part must be outside ifdef protection */
#include <lttng/tracepoint-event.h>
