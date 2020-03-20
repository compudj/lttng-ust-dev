#ifndef _LTTNG_MSGPACK_H
#define _LTTNG_MSGPACK_H

#include <stddef.h>
#include <stdint.h>

struct lttng_msgpack_writer {
	uint8_t *buffer;
	uint8_t *write_pos;
	const uint8_t *end_write_pos;
};

void lttng_msgpack_writer_init(
		struct lttng_msgpack_writer *writer, uint8_t *buffer, size_t size);

void lttng_msgpack_writer_fini(struct lttng_msgpack_writer *writer);

int lttng_msgpack_write_nil(struct lttng_msgpack_writer *writer);
int lttng_msgpack_write_true(struct lttng_msgpack_writer *writer);
int lttng_msgpack_write_false(struct lttng_msgpack_writer *writer);
int lttng_msgpack_write_u64(
		struct lttng_msgpack_writer *writer, uint64_t value);
int lttng_msgpack_write_i64(
		struct lttng_msgpack_writer *writer, int64_t value);
int lttng_msgpack_write_f64(struct lttng_msgpack_writer *writer, double value);
int lttng_msgpack_write_str(struct lttng_msgpack_writer *writer,
		const char *value);
int lttng_msgpack_begin_map(struct lttng_msgpack_writer *writer, size_t count);
int lttng_msgpack_end_map(struct lttng_msgpack_writer *writer);
int lttng_msgpack_begin_array(
		struct lttng_msgpack_writer *writer, size_t count);
int lttng_msgpack_end_array(struct lttng_msgpack_writer *writer);

#endif /* _LTTNG_MSGPACK_H */
