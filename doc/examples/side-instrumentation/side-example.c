/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * A libside-instrumented application.
 *
 * This program depends on libside only: it does not link against
 * LTTng-UST, and it does not know a tracer exists. Run it on its own
 * and its events cost a predicted-not-taken branch each. Run it with
 * liblttng-ust preloaded, as run-side-example does, and LTTng
 * subscribes to those same events and records them.
 *
 * The four events below are each about one thing:
 *
 *   side_example:reading   the scalar types, and integers and floats
 *                          which keep the byte order they are emitted
 *                          with rather than being converted to the
 *                          host's
 *   side_example:frame     the compound types: a structure, an array
 *                          of structures each holding an array, a
 *                          sequence of structures each holding a
 *                          sequence, and a variant
 *   side_example:snapshot  the gather types: fields read straight from
 *                          the memory of the application, described by
 *                          their offset, rather than copied onto the
 *                          argument vector by the caller
 *   side_example:packet    the attributes, which say how a field is
 *                          meant to be read rather than what it holds
 */

#include <side/trace.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * side_example:reading
 * --------------------
 */

static side_define_enum(example_states,
	side_enum_mapping_list(
		side_enum_mapping_value("idle", 0),
		side_enum_mapping_value("warming", 1),
		side_enum_mapping_range("running", 2, 9),
	)
);

side_static_event(reading_event, "side_example", "reading", SIDE_LOGLEVEL_INFO,
	side_field_list(
		side_field_u32("seq"),
		side_field_string("sensor"),
		side_field_enum("state", &example_states, side_elem(side_type_u32())),
		side_field_float_binary64("celsius"),
		side_field_bool("calibrated"),
		/*
		 * The same logical value, held in memory in three byte
		 * orders. Each is recorded exactly as it is emitted and
		 * the metadata says which order it is in, so a trace
		 * carries the bytes which were on the wire. A filter
		 * comparing one of these converts it to the byte order
		 * of the host first: "id_be == 4097" matches the
		 * reading whose sequence number is 1, on any host.
		 */
		side_field_u32("id_host"),
		side_field_u32_le("id_le"),
		side_field_u32_be("id_be"),
		side_field_float_binary32_be("volts_be"),
	)
);

/*
 * Put a host-order value into a given byte order. Only the order which
 * differs from the host's costs a swap.
 */
#if SIDE_BYTE_ORDER == SIDE_LITTLE_ENDIAN
# define TO_LE32(v)	(v)
# define TO_BE32(v)	side_bswap_32(v)
#else
# define TO_LE32(v)	side_bswap_32(v)
# define TO_BE32(v)	(v)
#endif

static float to_be_binary32(float f)
{
	union { float f; uint32_t u; } v = { .f = f };

	v.u = TO_BE32(v.u);
	return v.f;
}

static void emit_readings(void)
{
	static const char * const names[] = { "cpu0", "cpu1", "gpu0" };
	static const double celsius[] = { 41.5, 58.25, 72.0 };
	static const uint32_t states[] = { 1, 2, 5 };
	unsigned int i;

	for (i = 0; i < 3; i++) {
		uint32_t id = 0x1000 + i;
		float volts = 1.25f * (float) (i + 1);

		side_event(reading_event, side_arg_list(
			side_arg_u32(i),
			side_arg_string(names[i]),
			side_arg_u32(states[i]),
			side_arg_float_binary64(celsius[i]),
			side_arg_bool(i != 0),
			side_arg_u32(id),
			side_arg_u32(TO_LE32(id)),
			side_arg_u32(TO_BE32(id)),
			side_arg_float_binary32(to_be_binary32(volts))));
	}
}

/*
 * side_example:frame
 * ------------------
 */

/* struct { u32 magic; u16 version; } */
static side_define_struct(frame_header,
	side_field_list(
		side_field_u32("magic"),
		side_field_u16("version"),
	)
);

/* Three samples per channel. */
static side_define_array(sample_array, side_elem(side_type_u32()), 3);

/* struct { u32 id; u32 samples[3]; } */
static side_define_struct(channel_struct,
	side_field_list(
		side_field_u32("id"),
		side_field_array("samples", sample_array),
	)
);

/* An array of structures, each of which holds an array. */
static side_define_array(channel_array,
	side_elem(side_type_struct(channel_struct)), 2);

/* A sequence of u32, its length recorded as a u16. */
static side_define_vla(point_vla,
	side_elem(side_type_u32()),
	side_elem(side_type_u16()));

/* struct { u32 mark; sequence<u32> points; } */
static side_define_struct(burst_struct,
	side_field_list(
		side_field_u32("mark"),
		side_field_vla("points", point_vla),
	)
);

/* A sequence of structures, each of which holds a sequence. */
static side_define_vla(burst_vla,
	side_elem(side_type_struct(burst_struct)),
	side_elem(side_type_u16()));

/*
 * A variant: the option recorded is the one whose range contains the
 * value of the selector. Only the selected option occupies the trace.
 */
static side_define_variant(payload_variant,
	side_type_u32(),
	side_option_list(
		side_option_range(0, 0, side_type_u32()),
		side_option_range(1, 1, side_type_string()),
	)
);

side_static_event(frame_event, "side_example", "frame", SIDE_LOGLEVEL_INFO,
	side_field_list(
		side_field_struct("header", frame_header),
		side_field_array("channels", channel_array),
		side_field_vla("bursts", burst_vla),
		side_field_variant("payload", payload_variant),
	)
);

static void emit_frames(void)
{
	/* First frame: two bursts, of two and one point. */
	{
		side_arg_define_struct(header,
			side_arg_list(side_arg_u32(0xCAFE), side_arg_u16(1)));
		side_arg_define_array(samples_a,
			side_arg_list(side_arg_u32(10), side_arg_u32(20), side_arg_u32(30)));
		side_arg_define_array(samples_b,
			side_arg_list(side_arg_u32(40), side_arg_u32(50), side_arg_u32(60)));
		side_arg_define_struct(channel_a,
			side_arg_list(side_arg_u32(1), side_arg_array(samples_a)));
		side_arg_define_struct(channel_b,
			side_arg_list(side_arg_u32(2), side_arg_array(samples_b)));
		side_arg_define_array(channels,
			side_arg_list(side_arg_struct(channel_a), side_arg_struct(channel_b)));
		side_arg_define_vla(points_a,
			side_arg_list(side_arg_u32(101), side_arg_u32(102)));
		side_arg_define_vla(points_b,
			side_arg_list(side_arg_u32(103)));
		side_arg_define_struct(burst_a,
			side_arg_list(side_arg_u32(1), side_arg_vla(points_a)));
		side_arg_define_struct(burst_b,
			side_arg_list(side_arg_u32(2), side_arg_vla(points_b)));
		side_arg_define_vla(bursts,
			side_arg_list(side_arg_struct(burst_a), side_arg_struct(burst_b)));
		side_arg_define_variant(payload,
			side_arg_u32(0), side_arg_u32(0xC0FFEE));

		side_event(frame_event, side_arg_list(
			side_arg_struct(header),
			side_arg_array(channels),
			side_arg_vla(bursts),
			side_arg_variant(payload)));
	}

	/*
	 * Second frame: one burst of three points, and the other
	 * option of the variant. The two frames share one event
	 * description and record a different number of bytes.
	 */
	{
		side_arg_define_struct(header,
			side_arg_list(side_arg_u32(0xCAFE), side_arg_u16(2)));
		side_arg_define_array(samples_a,
			side_arg_list(side_arg_u32(11), side_arg_u32(21), side_arg_u32(31)));
		side_arg_define_array(samples_b,
			side_arg_list(side_arg_u32(41), side_arg_u32(51), side_arg_u32(61)));
		side_arg_define_struct(channel_a,
			side_arg_list(side_arg_u32(3), side_arg_array(samples_a)));
		side_arg_define_struct(channel_b,
			side_arg_list(side_arg_u32(4), side_arg_array(samples_b)));
		side_arg_define_array(channels,
			side_arg_list(side_arg_struct(channel_a), side_arg_struct(channel_b)));
		side_arg_define_vla(points,
			side_arg_list(side_arg_u32(201), side_arg_u32(202), side_arg_u32(203)));
		side_arg_define_struct(burst,
			side_arg_list(side_arg_u32(9), side_arg_vla(points)));
		side_arg_define_vla(bursts,
			side_arg_list(side_arg_struct(burst)));
		side_arg_define_variant(payload,
			side_arg_u32(1), side_arg_string("degraded"));

		side_event(frame_event, side_arg_list(
			side_arg_struct(header),
			side_arg_array(channels),
			side_arg_vla(bursts),
			side_arg_variant(payload)));
	}
}

/*
 * side_example:snapshot
 * ---------------------
 *
 * The application does not lay its state out for the tracer: it
 * describes the layout it already has. Each field says where to read
 * itself from, as an offset from a base address, and whether that
 * address is the value or a pointer to it.
 */

struct axis {
	uint32_t axis_id;
	int32_t reading;
};

struct sensor {
	uint32_t id;
	char name[16];
	double value;
	uint8_t active;
	uint16_t nr_samples;
	const uint32_t *samples;
	uint16_t nr_axes;
	const struct axis *axes;
};

/* The members of one element of the "axes" sequence below. */
static side_define_struct(axis_desc,
	side_field_list(
		side_field_gather_unsigned_integer("axis_id",
			offsetof(struct axis, axis_id),
			side_struct_field_sizeof(struct axis, axis_id), 0, 0,
			SIDE_TYPE_GATHER_ACCESS_DIRECT),
		side_field_gather_signed_integer("reading",
			offsetof(struct axis, reading),
			side_struct_field_sizeof(struct axis, reading), 0, 0,
			SIDE_TYPE_GATHER_ACCESS_DIRECT),
	)
);

static side_define_struct(sensor_desc,
	side_field_list(
		side_field_gather_unsigned_integer("id",
			offsetof(struct sensor, id),
			side_struct_field_sizeof(struct sensor, id), 0, 0,
			SIDE_TYPE_GATHER_ACCESS_DIRECT),
		side_field_gather_string("name",
			offsetof(struct sensor, name),
			SIDE_TYPE_GATHER_ACCESS_DIRECT),
		side_field_gather_float("value",
			offsetof(struct sensor, value),
			side_struct_field_sizeof(struct sensor, value),
			SIDE_TYPE_GATHER_ACCESS_DIRECT),
		side_field_gather_bool("active",
			offsetof(struct sensor, active),
			side_struct_field_sizeof(struct sensor, active), 0, 0,
			SIDE_TYPE_GATHER_ACCESS_DIRECT),
		/*
		 * A sequence whose elements live behind a pointer and
		 * whose length is itself read from the structure.
		 */
		side_field_gather_vla("samples",
			side_elem(side_type_gather_unsigned_integer(0,
				sizeof(uint32_t), 0, 0,
				SIDE_TYPE_GATHER_ACCESS_DIRECT)),
			offsetof(struct sensor, samples),
			SIDE_TYPE_GATHER_ACCESS_POINTER,
			side_length(side_type_gather_unsigned_integer(
				offsetof(struct sensor, nr_samples),
				side_struct_field_sizeof(struct sensor, nr_samples),
				0, 0, SIDE_TYPE_GATHER_ACCESS_DIRECT))),
		/*
		 * A sequence of structures, gathered: each element is
		 * read from the application array one element size
		 * apart, and each of its members applies its own offset
		 * to that element. Nothing is marshalled by the caller.
		 */
		side_field_gather_vla("axes",
			side_elem(side_type_gather_struct(axis_desc, 0,
				sizeof(struct axis),
				SIDE_TYPE_GATHER_ACCESS_DIRECT)),
			offsetof(struct sensor, axes),
			SIDE_TYPE_GATHER_ACCESS_POINTER,
			side_length(side_type_gather_unsigned_integer(
				offsetof(struct sensor, nr_axes),
				side_struct_field_sizeof(struct sensor, nr_axes),
				0, 0, SIDE_TYPE_GATHER_ACCESS_DIRECT))),
	)
);

side_static_event(snapshot_event, "side_example", "snapshot", SIDE_LOGLEVEL_INFO,
	side_field_list(
		side_field_gather_struct("sensor", sensor_desc, 0,
			sizeof(struct sensor), SIDE_TYPE_GATHER_ACCESS_DIRECT),
		side_field_gather_array("window",
			side_elem(side_type_gather_unsigned_integer(0,
				sizeof(uint32_t), 0, 0,
				SIDE_TYPE_GATHER_ACCESS_DIRECT)),
			4, 0, SIDE_TYPE_GATHER_ACCESS_DIRECT),
	)
);

static void emit_snapshots(void)
{
	static const uint32_t samples_cpu0[] = { 5, 15, 25 };
	static const uint32_t samples_cpu1[] = { 60, 120 };
	static const uint32_t samples_gpu0[] = { 300, 600, 900, 1200 };
	static const struct axis axes_cpu0[] = { { 0, -3 }, { 1, 7 } };
	static const struct axis axes_cpu1[] = { { 0, 12 } };
	static const struct axis axes_gpu0[] = { { 0, -1 }, { 1, 0 }, { 2, 42 } };
	struct sensor sensors[] = {
		{ 1, "cpu0", 41.5, 1, 3, samples_cpu0, 2, axes_cpu0 },
		{ 2, "cpu1", 58.25, 1, 2, samples_cpu1, 1, axes_cpu1 },
		{ 3, "gpu0", 72.0, 0, 4, samples_gpu0, 3, axes_gpu0 },
	};
	uint32_t window[4] = { 7, 14, 21, 28 };
	unsigned int i;

	for (i = 0; i < 3; i++) {
		/*
		 * Nothing is copied here: the two arguments are the
		 * addresses the description reads from.
		 */
		side_event(snapshot_event, side_arg_list(
			side_arg_gather_struct(&sensors[i]),
			side_arg_gather_array(&window)));
	}
}

/*
 * side_example:packet
 * -------------------
 *
 * An attribute says how a field is meant to be read, which its type
 * does not: the addresses below are an unsigned integer and sixteen
 * bytes, and nothing about those types makes them network addresses.
 * The attribute travels with the description into the metadata of the
 * trace, where a consumer which knows it renders the value as an
 * address, and one which does not still decodes the field.
 */

/*
 * The sixteen bytes of an IPv6 address. Their element type is the byte
 * rather than an integer, which is what makes the tracer record the
 * array as a blob.
 */
static side_define_array(ipv6_addr, side_elem(side_type_byte()), 16,
	side_attr_list(side_attr("lttng.fmt.ipv6", side_attr_bool(true))));

/*
 * An address is not always a field of the event: this one is a member
 * of a structure, and carries its attribute just the same.
 *
 * struct { u32 addr; u16 port; }
 */
static side_define_struct(endpoint_struct,
	side_field_list(
		side_field_u32_be("addr",
			side_attr_list(side_attr("lttng.fmt.ipv4", side_attr_bool(true)))),
		side_field_u16("port"),
	)
);

side_static_event(packet_event, "side_example", "packet", SIDE_LOGLEVEL_INFO,
	side_field_list(
		side_field_u32("seq"),
		/*
		 * An IPv4 address is four bytes in network byte order.
		 * Declared big endian, the trace holds the bytes which
		 * were on the wire whatever the byte order of the host,
		 * and a reader converts them once.
		 */
		side_field_u32_be("saddr",
			side_attr_list(side_attr("lttng.fmt.ipv4", side_attr_bool(true)))),
		side_field_u32_be("daddr",
			side_attr_list(side_attr("lttng.fmt.ipv4", side_attr_bool(true)))),
		side_field_array("saddr6", ipv6_addr),
		side_field_array("daddr6", ipv6_addr),
		side_field_struct("peer", endpoint_struct),
		side_field_u16("length"),
	)
);

static void parse_ipv4(const char *str, struct in_addr *addr)
{
	if (inet_pton(AF_INET, str, addr) != 1) {
		fprintf(stderr, "not an IPv4 address: %s\n", str);
		exit(EXIT_FAILURE);
	}
}

static void parse_ipv6(const char *str, struct in6_addr *addr)
{
	if (inet_pton(AF_INET6, str, addr) != 1) {
		fprintf(stderr, "not an IPv6 address: %s\n", str);
		exit(EXIT_FAILURE);
	}
}

static void emit_packets(void)
{
	static const struct {
		const char *saddr, *daddr, *saddr6, *daddr6, *peer;
		uint16_t port, length;
	} packets[] = {
		{ "10.0.0.1", "192.168.1.1", "2001:db8::1", "2001:db8::2",
			"10.0.0.254", 443, 1500 },
		/* The destination is the same host over both families. */
		{ "172.16.5.4", "8.8.8.8", "fe80::1", "::ffff:8.8.8.8",
			"172.16.5.1", 53, 590 },
		{ "127.0.0.1", "255.255.255.255", "::", "ff02::1",
			"127.0.0.1", 9, 64 },
	};
	unsigned int i;

	for (i = 0; i < 3; i++) {
		struct in_addr saddr, daddr, peer;
		struct in6_addr saddr6, daddr6;

		parse_ipv4(packets[i].saddr, &saddr);
		parse_ipv4(packets[i].daddr, &daddr);
		parse_ipv4(packets[i].peer, &peer);
		parse_ipv6(packets[i].saddr6, &saddr6);
		parse_ipv6(packets[i].daddr6, &daddr6);

		{
			/*
			 * An argument list is a literal: the sixteen
			 * bytes of an address are written out rather
			 * than pushed in a loop.
			 */
			side_arg_define_array(arg_saddr6, side_arg_list(
				side_arg_byte(saddr6.s6_addr[0]), side_arg_byte(saddr6.s6_addr[1]),
				side_arg_byte(saddr6.s6_addr[2]), side_arg_byte(saddr6.s6_addr[3]),
				side_arg_byte(saddr6.s6_addr[4]), side_arg_byte(saddr6.s6_addr[5]),
				side_arg_byte(saddr6.s6_addr[6]), side_arg_byte(saddr6.s6_addr[7]),
				side_arg_byte(saddr6.s6_addr[8]), side_arg_byte(saddr6.s6_addr[9]),
				side_arg_byte(saddr6.s6_addr[10]), side_arg_byte(saddr6.s6_addr[11]),
				side_arg_byte(saddr6.s6_addr[12]), side_arg_byte(saddr6.s6_addr[13]),
				side_arg_byte(saddr6.s6_addr[14]), side_arg_byte(saddr6.s6_addr[15])));
			side_arg_define_array(arg_daddr6, side_arg_list(
				side_arg_byte(daddr6.s6_addr[0]), side_arg_byte(daddr6.s6_addr[1]),
				side_arg_byte(daddr6.s6_addr[2]), side_arg_byte(daddr6.s6_addr[3]),
				side_arg_byte(daddr6.s6_addr[4]), side_arg_byte(daddr6.s6_addr[5]),
				side_arg_byte(daddr6.s6_addr[6]), side_arg_byte(daddr6.s6_addr[7]),
				side_arg_byte(daddr6.s6_addr[8]), side_arg_byte(daddr6.s6_addr[9]),
				side_arg_byte(daddr6.s6_addr[10]), side_arg_byte(daddr6.s6_addr[11]),
				side_arg_byte(daddr6.s6_addr[12]), side_arg_byte(daddr6.s6_addr[13]),
				side_arg_byte(daddr6.s6_addr[14]), side_arg_byte(daddr6.s6_addr[15])));
			/*
			 * s_addr already holds the four bytes in
			 * network byte order, which is what the big
			 * endian field of the description expects.
			 */
			side_arg_define_struct(arg_peer, side_arg_list(
				side_arg_u32(peer.s_addr),
				side_arg_u16(packets[i].port)));

			side_event(packet_event, side_arg_list(
				side_arg_u32(i),
				side_arg_u32(saddr.s_addr),
				side_arg_u32(daddr.s_addr),
				side_arg_array(arg_saddr6),
				side_arg_array(arg_daddr6),
				side_arg_struct(arg_peer),
				side_arg_u16(packets[i].length)));
		}
	}
}

int main(void)
{
	emit_readings();
	printf("emitted 3 side_example:reading events\n");
	emit_frames();
	printf("emitted 2 side_example:frame events\n");
	emit_snapshots();
	printf("emitted 3 side_example:snapshot events\n");
	emit_packets();
	printf("emitted 3 side_example:packet events\n");
	return 0;
}
