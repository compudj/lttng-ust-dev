#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER ust_tests_ctf_types

#if !defined(_TRACEPOINT_UST_TESTS_CTF_TYPES_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _TRACEPOINT_UST_TESTS_CTF_TYPES_H

/*
 * Copyright (C) 2014 Geneviève Bastien <gbastien@versatic.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <lttng/tracepoint.h>

TRACEPOINT_ENUM(ust_tests_ctf_types, testenum,
	TP_ENUM_VALUES(
		ctf_enum_value("even", 0)
		ctf_enum_value("uneven", 1)
		ctf_enum_range("twoto4", 2, 4)
		ctf_enum_value("five\"extra\\test", 5)
	)
)

TRACEPOINT_ENUM(ust_tests_ctf_types, testenum2,
	TP_ENUM_VALUES(
		ctf_enum_value("zero", 0)
		ctf_enum_value("five", 5)
		ctf_enum_range("ten_to_twenty", 10, 20)
	)
)

/*
 * Enumeration field is used twice to make sure the type declaration
 * is entered only once in the metadata file.
 */
TRACEPOINT_EVENT(ust_tests_ctf_types, tptest,
	TP_ARGS(int, anint, int, enumval, int, enumval2),
	TP_FIELDS(
		ctf_integer(int, intfield, anint)
		ctf_enum(ust_tests_ctf_types, testenum, int, enumfield, enumval)
		ctf_enum(ust_tests_ctf_types, testenum, long long,
				enumfield_bis, enumval)
		ctf_enum(ust_tests_ctf_types, testenum2, unsigned int,
				enumfield_third, enumval2)
	)
)

/*
 * Another tracepoint using the types to make sure each type is entered
 * only once in the metadata file.
 */
TRACEPOINT_EVENT(ust_tests_ctf_types, tptest_bis,
	TP_ARGS(int, anint, int, enumval),
	TP_FIELDS(
		ctf_integer(int, intfield, anint)
		ctf_enum(ust_tests_ctf_types, testenum, unsigned char,
			enumfield, enumval)
	)
)

#endif /* _TRACEPOINT_UST_TESTS_CTF_TYPES_H */

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "./ust_tests_ctf_types.h"

/* This part must be outside ifdef protection */
#include <lttng/tracepoint-event.h>
