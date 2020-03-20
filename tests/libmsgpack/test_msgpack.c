#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tap.h"

#include "../libmsgpack/msgpack-internal.h"

#define BUFFER_SIZE 4096
#define NUM_TESTS 9

static void string_test(uint8_t *buf, const char *value)
{
	struct lttng_msgpack_writer writer;

	lttng_msgpack_writer_init(&writer, buf, BUFFER_SIZE);
	lttng_msgpack_write_str(&writer, value);
	lttng_msgpack_writer_fini(&writer);
}

static void int_test(uint8_t *buf, int64_t value)
{
	struct lttng_msgpack_writer writer;

	lttng_msgpack_writer_init(&writer, buf, BUFFER_SIZE);
	lttng_msgpack_write_i64(&writer, value);

	lttng_msgpack_writer_fini(&writer);
}

static void uint_test(uint8_t *buf, uint64_t value)
{
	struct lttng_msgpack_writer writer;

	lttng_msgpack_writer_init(&writer, buf, BUFFER_SIZE);
	lttng_msgpack_write_u64(&writer, value);
	lttng_msgpack_writer_fini(&writer);
}

static void float_test(uint8_t *buf, double value)
{
	struct lttng_msgpack_writer writer;

	lttng_msgpack_writer_init(&writer, buf, BUFFER_SIZE);
	lttng_msgpack_write_f64(&writer, value);
	lttng_msgpack_writer_fini(&writer);
}

static void array_double_test(uint8_t *buf, double *values, size_t nb_values)
{
	int i = 0;
	struct lttng_msgpack_writer writer;

	lttng_msgpack_writer_init(&writer, buf, BUFFER_SIZE);
	lttng_msgpack_begin_array(&writer, nb_values);

	for (i = 0; i < nb_values; i++) {
		lttng_msgpack_write_f64(&writer, values[i]);
	}

	lttng_msgpack_end_array(&writer);
	lttng_msgpack_writer_fini(&writer);
}

static void complete_capture_test(uint8_t *buf)
{
	/*
	 * This testcase tests the following json representation:
	 * {"id":17,"captures":["meow mix",18, null, 14.197,[1980, 1995]]}
	 */
	struct lttng_msgpack_writer writer;

	lttng_msgpack_writer_init(&writer, buf, BUFFER_SIZE);
	lttng_msgpack_begin_map(&writer, 2);

	lttng_msgpack_write_str(&writer, "id");
	lttng_msgpack_write_u64(&writer, 17);

	lttng_msgpack_write_str(&writer, "captures");
	lttng_msgpack_begin_array(&writer, 4);

	lttng_msgpack_write_str(&writer, "meow mix");
	lttng_msgpack_write_u64(&writer, 18);
	lttng_msgpack_write_nil(&writer);
	lttng_msgpack_write_f64(&writer, 14.197);

	lttng_msgpack_begin_array(&writer, 2);

	lttng_msgpack_write_u64(&writer, 1980);
	lttng_msgpack_write_u64(&writer, 1995);

	lttng_msgpack_end_array(&writer);

	lttng_msgpack_end_array(&writer);

	lttng_msgpack_end_map(&writer);
	lttng_msgpack_writer_fini(&writer);
}

static void nil_test(uint8_t *buf)
{
	struct lttng_msgpack_writer writer;

	lttng_msgpack_writer_init(&writer, buf, BUFFER_SIZE);
	lttng_msgpack_write_nil(&writer);
	lttng_msgpack_writer_fini(&writer);
}

int main(int argc, char *argv[])
{
	uint8_t buf[BUFFER_SIZE] = {0};
	double arr_double[] = {1.1, 2.3, -12345.2};

	plan_tests(NUM_TESTS);
	/* We use `#define` here as we can use `sizeof` on literal strings. */

	diag("Testing msgpack implementation");

	/*
	 * Expected outputs were produced using the `json2msgpack` tool.
	 * https://github.com/ludocode/msgpack-tools
	 * For example, here is the command to produce the null test expected
	 * output:
	 *  echo 'null' | json2msgpack | hexdump -v -e '"\\\x" 1/1 "%02x"'
	 *
	 * The only exception is that we always produce 64bits integer to
	 * represent integers even if they would fit into smaller objects so
	 * they need to be manually crafted in 64bits two's complement (if
	 * signed) big endian.
	 */
	#define NIL_EXPECTED "\xc0"
	nil_test(buf);
	ok(memcmp(buf, NIL_EXPECTED, sizeof(NIL_EXPECTED)) == 0,
		"NIL object");

	#define STRING_BYE_EXPECTED "\xa3\x62\x79\x65"
	string_test(buf, "bye");
	ok(memcmp(buf, STRING_BYE_EXPECTED, sizeof(STRING_BYE_EXPECTED)) == 0,
		"String \"bye\" object");

	#define U64_1337_EXPECTED "\xcf\x00\x00\x00\x00\x00\x00\x05\x39"
	uint_test(buf, 1337);
	ok(memcmp(buf, U64_1337_EXPECTED, sizeof(U64_1337_EXPECTED)) == 0,
		"u64 \"1337\" object");

	#define S64_MINUS_4242_EXPECTED "\xd3\xff\xff\xff\xff\xff\xff\xef\x6e"
	int_test(buf, -4242);
	ok(memcmp(buf, S64_MINUS_4242_EXPECTED, sizeof(S64_MINUS_4242_EXPECTED)) == 0,
		"u64 \"-4242\" object");

	#define F64_ZERO_EXPECTED "\xcb\x00\x00\x00\x00\x00\x00\x00\x00"
	float_test(buf, 0.0);
	ok(memcmp(buf, F64_ZERO_EXPECTED, sizeof(F64_ZERO_EXPECTED)) == 0,
		"f64 \"0.0\" object");

	#define F64_PI_EXPECTED "\xcb\x40\x09\x21\xfb\x53\xc8\xd4\xf1"
	float_test(buf, 3.14159265);
	ok(memcmp(buf, F64_PI_EXPECTED, sizeof(F64_PI_EXPECTED)) == 0,
		"f64 \"PI\" object");

	#define F64_MINUS_PI_EXPECTED "\xcb\xc0\x09\x21\xfb\x53\xc8\xd4\xf1"
	float_test(buf, -3.14159265);
	ok(memcmp(buf, F64_MINUS_PI_EXPECTED, sizeof(F64_MINUS_PI_EXPECTED)) == 0,
		"f64 \"-PI\" object");

	#define ARRAY_DOUBLE_EXPECTED "\x93" \
			"\xcb\x3f\xf1\x99\x99\x99\x99\x99\x9a" \
			"\xcb\x40\x02\x66\x66\x66\x66\x66\x66" \
			"\xcb\xc0\xc8\x1c\x99\x99\x99\x99\x9a"
	array_double_test(buf, arr_double, 3);
	ok(memcmp(buf, ARRAY_DOUBLE_EXPECTED, sizeof(ARRAY_DOUBLE_EXPECTED)) == 0,
		"Array of double object");

	/*
	 * fixmap size 2
	 * fixstr size 2, "id"
	 * u64, 17
	 * fixstr size 8, "captures"
	 * fixarray size 4
	 * fixstr size 8, "meow mix"
	 * u64, 18
	 * NIL
	 * f64, 14.197
	 * fixarray size 2
	 * u64, 1980
	 * u64, 1995
	 */
	#define COMPLETE_CAPTURE_EXPECTED "\x82"  \
		"\xa2\x69\x64" \
		"\xcf\x00\x00\x00\x00\x00\x00\x00\x11" \
		"\xa8\x63\x61\x70\x74\x75\x72\x65\x73" \
		"\x94" \
			"\xa8\x6d\x65\x6f\x77\x20\x6d\x69\x78" \
			"\xcf\x00\x00\x00\x00\x00\x00\x00\x12" \
			"\xc0"					\
			"\xcb\x40\x2c\x64\xdd\x2f\x1a\x9f\xbe"	\
			"\x92" \
				"\xcf\x00\x00\x00\x00\x00\x00\x07\xbc" \
				"\xcf\x00\x00\x00\x00\x00\x00\x07\xcb"

	complete_capture_test(buf);
	ok(memcmp(buf, COMPLETE_CAPTURE_EXPECTED, sizeof(COMPLETE_CAPTURE_EXPECTED)) == 0,
		"Complete capture object");

	return EXIT_SUCCESS;
}
