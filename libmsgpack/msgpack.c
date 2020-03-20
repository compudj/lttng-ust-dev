#include <assert.h>
#include <endian.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "msgpack.h"

#define MSGPACK_FIXSTR_ID_MASK		0xA0
#define MSGPACK_FIXMAP_ID_MASK		0x80
#define MSGPACK_FIXARRAY_ID_MASK	0x90

#define MSGPACK_NIL_ID		0xC0
#define MSGPACK_FALSE_ID	0xC2
#define MSGPACK_TRUE_ID		0xC3
#define MSGPACK_MAP16_ID	0xDE
#define MSGPACK_ARRAY16_ID	0xDC
#define MSGPACK_UINT64_ID	0xCF
#define MSGPACK_INT64_ID	0xD3
#define MSGPACK_FLOAT64_ID	0xCB
#define MSGPACK_STR16_ID	0xDA

#define MSGPACK_FIXMAP_MAX_COUNT	15
#define MSGPACK_FIXARRAY_MAX_COUNT	15
#define MSGPACK_FIXSTR_MAX_LENGTH	31

static inline int lttng_msgpack_append_buffer(
		struct lttng_msgpack_writer *writer,
		const uint8_t *buf,
		size_t length)
{
	int ret = 0;

	assert(buf);
	if (writer->write_pos + length > writer->end_write_pos) {
		ret = -1;
		goto end;
	}

	memcpy(writer->write_pos, buf, length);
	writer->write_pos += length;
end:
	return ret;
}

static inline int lttng_msgpack_append_u8(
		struct lttng_msgpack_writer *writer, uint8_t value)
{
	return lttng_msgpack_append_buffer(writer, &value, sizeof(value));
}

static inline int lttng_msgpack_append_u16(
		struct lttng_msgpack_writer *writer, uint16_t value)
{
	value = htobe16(value);

	return lttng_msgpack_append_buffer(writer, (uint8_t *) &value, sizeof(value));
}

static inline int lttng_msgpack_append_u64(
		struct lttng_msgpack_writer *writer, uint64_t value)
{
	value = htobe64(value);

	return lttng_msgpack_append_buffer(writer, (uint8_t *) &value, sizeof(value));
}
static inline int lttng_msgpack_append_f64(
		struct lttng_msgpack_writer *writer, double value)
{

	union {
		double d;
		uint64_t u;
	} u;

	u.d = value;

	return lttng_msgpack_append_u64(writer, u.u);
}

static inline int lttng_msgpack_append_i64(
		struct lttng_msgpack_writer *writer, int64_t value)
{
	return lttng_msgpack_append_u64(writer, (uint64_t) value);
}

static inline int lttng_msgpack_encode_u64(
		struct lttng_msgpack_writer *writer, uint64_t value)
{
	int ret;

	ret = lttng_msgpack_append_u8(writer, MSGPACK_UINT64_ID);
	if (ret)
		goto end;

	ret = lttng_msgpack_append_u64(writer, value);
	if (ret)
		goto end;

end:
	return ret;
}

static inline int lttng_msgpack_encode_i64(
		struct lttng_msgpack_writer *writer, int64_t value)
{
	int ret;

	ret = lttng_msgpack_append_u8(writer, MSGPACK_INT64_ID);
	if (ret)
		goto end;

	ret = lttng_msgpack_append_i64(writer, value);
	if (ret)
		goto end;

end:
	return ret;
}

static inline int lttng_msgpack_encode_f64(
		struct lttng_msgpack_writer *writer, double value)
{
	int ret;

	ret = lttng_msgpack_append_u8(writer, MSGPACK_FLOAT64_ID);
	if (ret)
		goto end;

	ret = lttng_msgpack_append_f64(writer, value);
	if (ret)
		goto end;

end:
	return ret;
}

static inline int lttng_msgpack_encode_fixmap(
		struct lttng_msgpack_writer *writer, uint8_t count)
{
	int ret = 0;

	assert(count <= MSGPACK_FIXMAP_MAX_COUNT);

	ret = lttng_msgpack_append_u8(writer, MSGPACK_FIXMAP_ID_MASK | count);
	if (ret)
		goto end;

end:
	return ret;
}

static inline int lttng_msgpack_encode_map16(
		struct lttng_msgpack_writer *writer, uint16_t count)
{
	int ret;

	assert(count > MSGPACK_FIXMAP_MAX_COUNT);

	ret = lttng_msgpack_append_u8(writer, MSGPACK_MAP16_ID);
	if (ret)
		goto end;

	ret = lttng_msgpack_append_u16(writer, count);
	if (ret)
		goto end;

end:
	return ret;
}

static inline int lttng_msgpack_encode_fixarray(
		struct lttng_msgpack_writer *writer, uint8_t count)
{
	int ret = 0;

	assert(count <= MSGPACK_FIXARRAY_MAX_COUNT);

	ret = lttng_msgpack_append_u8(writer, MSGPACK_FIXARRAY_ID_MASK | count);
	if (ret)
		goto end;

end:
	return ret;
}

static inline int lttng_msgpack_encode_array16(
		struct lttng_msgpack_writer *writer, uint16_t count)
{
	int ret;

	assert(count > MSGPACK_FIXARRAY_MAX_COUNT);

	ret = lttng_msgpack_append_u8(writer, MSGPACK_ARRAY16_ID);
	if (ret)
		goto end;

	ret = lttng_msgpack_append_u16(writer, count);
	if (ret)
		goto end;

end:
	return ret;
}

static inline int lttng_msgpack_encode_fixstr(
		struct lttng_msgpack_writer *writer,
		const char *str,
		uint8_t len)
{
	int ret;

	assert(len <= MSGPACK_FIXSTR_MAX_LENGTH);

	ret = lttng_msgpack_append_u8(writer, MSGPACK_FIXSTR_ID_MASK | len);
	if (ret)
		goto end;

	ret = lttng_msgpack_append_buffer(writer, (uint8_t *) str, len);
	if (ret)
		goto end;

end:
	return ret;
}

static inline int lttng_msgpack_encode_str16(
		struct lttng_msgpack_writer *writer,
		const char *str,
		uint16_t len)
{
	int ret;

	assert(len > MSGPACK_FIXSTR_MAX_LENGTH);

	ret = lttng_msgpack_append_u8(writer, MSGPACK_STR16_ID);
	if (ret)
		goto end;

	ret = lttng_msgpack_append_buffer(writer, (uint8_t *) str, len);
	if (ret)
		goto end;

end:
	return ret;
}

int lttng_msgpack_begin_map(struct lttng_msgpack_writer *writer, size_t count)
{
	int ret;

	if (count < 0 || count >= (1 << 16)) {
		ret = -1;
		goto end;
	}

	if (count <= MSGPACK_FIXMAP_MAX_COUNT)
		ret = lttng_msgpack_encode_fixmap(writer, count);
	else
		ret = lttng_msgpack_encode_map16(writer, count);

end:
	return ret;
}

int lttng_msgpack_end_map(struct lttng_msgpack_writer *writer)
{
	// nothing for now I think
	// Sanity check later?
	return 0;
}

int lttng_msgpack_begin_array(
		struct lttng_msgpack_writer *writer, size_t count)
{
	int ret;

	if (count < 0 || count >= (1 << 16)) {
		ret = -1;
		goto end;
	}

	if (count <= MSGPACK_FIXARRAY_MAX_COUNT)
		ret = lttng_msgpack_encode_fixarray(writer, count);
	else
		ret = lttng_msgpack_encode_array16(writer, count);

end:
	return ret;
}

int lttng_msgpack_end_array(struct lttng_msgpack_writer *writer)
{
	// nothing for now I think
	// Sanity check later?
	return 0;
}

int lttng_msgpack_write_str(struct lttng_msgpack_writer *writer,
		const char *str)
{
	int ret;
	size_t length = strlen(str);
	if (length < 0 || length >= (1 << 16)) {
		ret = -1;
		goto end;
	}

	if (length <= MSGPACK_FIXSTR_MAX_LENGTH)
		ret = lttng_msgpack_encode_fixstr(writer, str, length);
	else
		ret = lttng_msgpack_encode_str16(writer, str, length);

end:
	return ret;
}

int lttng_msgpack_write_nil(struct lttng_msgpack_writer *writer)
{
	return lttng_msgpack_append_u8(writer, MSGPACK_NIL_ID);
}

int lttng_msgpack_write_true(struct lttng_msgpack_writer *writer)
{
	return lttng_msgpack_append_u8(writer, MSGPACK_TRUE_ID);
}

int lttng_msgpack_write_false(struct lttng_msgpack_writer *writer)
{
	return lttng_msgpack_append_u8(writer, MSGPACK_FALSE_ID);
}

int lttng_msgpack_write_u64(
		struct lttng_msgpack_writer *writer, uint64_t value)
{
	return lttng_msgpack_encode_u64(writer, value);
}

int lttng_msgpack_write_i64(struct lttng_msgpack_writer *writer, int64_t value)
{
	return lttng_msgpack_encode_i64(writer, value);
}

int lttng_msgpack_write_f64(struct lttng_msgpack_writer *writer, double value)
{
	return lttng_msgpack_encode_f64(writer, value);
}

void lttng_msgpack_writer_init(
		struct lttng_msgpack_writer *writer, uint8_t *buffer, size_t size)
{
	assert(buffer);
	assert(size >= 0);

	writer->buffer = buffer;
	writer->write_pos = buffer;
	writer->end_write_pos = buffer + size;
}

void lttng_msgpack_writer_fini(struct lttng_msgpack_writer *writer)
{
	memset(writer, 0, sizeof(*writer));
}
