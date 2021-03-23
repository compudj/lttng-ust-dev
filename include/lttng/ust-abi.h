/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2010-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * LTTng-UST ABI header
 */

#ifndef _LTTNG_UST_ABI_H
#define _LTTNG_UST_ABI_H

#include <stdint.h>
#include <lttng/ust-compiler.h>

#ifndef LTTNG_PACKED
#error "LTTNG_PACKED should be defined"
#endif

#ifndef __ust_stringify
#define __ust_stringify1(x)	#x
#define __ust_stringify(x)	__ust_stringify1(x)
#endif /* __ust_stringify */

#define LTTNG_UST_SYM_NAME_LEN			256
#define LTTNG_UST_ABI_PROCNAME_LEN		16

/* UST comm magic number, used to validate protocol and endianness. */
#define LTTNG_UST_COMM_MAGIC			0xC57C57C5

/* Version for ABI between liblttng-ust, sessiond, consumerd */
#define LTTNG_UST_ABI_MAJOR_VERSION			9
#define LTTNG_UST_ABI_MAJOR_VERSION_OLDEST_COMPATIBLE	8
#define LTTNG_UST_ABI_MINOR_VERSION		0

enum lttng_ust_instrumentation {
	LTTNG_UST_TRACEPOINT		= 0,
	LTTNG_UST_PROBE			= 1,
	LTTNG_UST_FUNCTION		= 2,
};

enum lttng_ust_loglevel_type {
	LTTNG_UST_LOGLEVEL_ALL		= 0,
	LTTNG_UST_LOGLEVEL_RANGE	= 1,
	LTTNG_UST_LOGLEVEL_SINGLE	= 2,
};

enum lttng_ust_output {
	LTTNG_UST_MMAP		= 0,
};

enum lttng_ust_chan_type {
	LTTNG_UST_CHAN_PER_CPU = 0,
	LTTNG_UST_CHAN_METADATA = 1,
};

struct lttng_ust_tracer_version {
	uint32_t major;
	uint32_t minor;
	uint32_t patchlevel;
} LTTNG_PACKED;

#define LTTNG_UST_CHANNEL_PADDING	(LTTNG_UST_SYM_NAME_LEN + 32)
/*
 * Given that the consumerd is limited to 64k file descriptors, we
 * cannot expect much more than 1MB channel structure size. This size is
 * depends on the number of streams within a channel, which depends on
 * the number of possible CPUs on the system.
 */
#define LTTNG_UST_CHANNEL_DATA_MAX_LEN	1048576U
struct lttng_ust_channel {
	uint64_t len;
	enum lttng_ust_chan_type type;
	char padding[LTTNG_UST_CHANNEL_PADDING];
	char data[];	/* variable sized data */
} LTTNG_PACKED;

#define LTTNG_UST_STREAM_PADDING1	(LTTNG_UST_SYM_NAME_LEN + 32)
struct lttng_ust_stream {
	uint64_t len;		/* shm len */
	uint32_t stream_nr;	/* stream number */
	char padding[LTTNG_UST_STREAM_PADDING1];
	/*
	 * shm_fd and wakeup_fd are send over unix socket as file
	 * descriptors after this structure.
	 */
} LTTNG_PACKED;

#define LTTNG_UST_EVENT_PADDING1	8
#define LTTNG_UST_EVENT_PADDING2	(LTTNG_UST_SYM_NAME_LEN + 32)
struct lttng_ust_event {
	enum lttng_ust_instrumentation instrumentation;
	char name[LTTNG_UST_SYM_NAME_LEN];	/* event name */

	enum lttng_ust_loglevel_type loglevel_type;
	int loglevel;	/* value, -1: all */
	uint64_t token;				/* User-provided token */
	char padding[LTTNG_UST_EVENT_PADDING1];

	/* Per instrumentation type configuration */
	union {
		char padding[LTTNG_UST_EVENT_PADDING2];
	} u;
} LTTNG_PACKED;

#define LTTNG_UST_EVENT_NOTIFIER_PADDING	32
struct lttng_ust_event_notifier {
	struct lttng_ust_event event;
	uint64_t error_counter_index;

	char padding[LTTNG_UST_EVENT_NOTIFIER_PADDING];
} LTTNG_PACKED;

#define LTTNG_UST_EVENT_NOTIFIER_NOTIFICATION_PADDING 32
struct lttng_ust_event_notifier_notification {
	uint64_t token;
	uint16_t capture_buf_size;
	char padding[LTTNG_UST_EVENT_NOTIFIER_NOTIFICATION_PADDING];
} LTTNG_PACKED;

enum lttng_ust_key_token_type {
	LTTNG_UST_KEY_TOKEN_STRING = 0,		/* arg: strtab_offset. */
	LTTNG_UST_KEY_TOKEN_EVENT_NAME = 1,	/* no arg. */
	LTTNG_UST_KEY_TOKEN_PROVIDER_NAME = 2,	/* no arg. */
};

#define LTTNG_UST_KEY_ARG_PADDING1		256
#define LTTNG_UST_KEY_TOKEN_STRING_LEN_MAX	256
struct lttng_ust_key_token {
	uint32_t type;	/* enum lttng_ust_key_token_type */
	union {
		char string[LTTNG_UST_KEY_TOKEN_STRING_LEN_MAX];
		char padding[LTTNG_UST_KEY_ARG_PADDING1];
	} arg;
} LTTNG_PACKED;

#define LTTNG_UST_NR_KEY_TOKEN 4
struct lttng_ust_counter_key_dimension {
	uint32_t nr_key_tokens;
	struct lttng_ust_key_token key_tokens[LTTNG_UST_NR_KEY_TOKEN];
} LTTNG_PACKED;

#define LTTNG_UST_COUNTER_DIMENSION_MAX 4
struct lttng_ust_counter_key {
	uint32_t nr_dimensions;
	struct lttng_ust_counter_key_dimension key_dimensions[LTTNG_UST_COUNTER_DIMENSION_MAX];
} LTTNG_PACKED;

#define LTTNG_UST_COUNTER_EVENT_PADDING1	16
struct lttng_ust_counter_event {
	struct lttng_ust_event event;
	struct lttng_ust_counter_key key;
	char padding[LTTNG_UST_COUNTER_EVENT_PADDING1];
} LTTNG_PACKED;

enum lttng_ust_counter_arithmetic {
	LTTNG_UST_COUNTER_ARITHMETIC_MODULAR = 0,
	LTTNG_UST_COUNTER_ARITHMETIC_SATURATION = 1,
};

enum lttng_ust_counter_bitness {
	LTTNG_UST_COUNTER_BITNESS_32 = 0,
	LTTNG_UST_COUNTER_BITNESS_64 = 1,
};

struct lttng_ust_counter_dimension {
	uint64_t size;
	uint64_t underflow_index;
	uint64_t overflow_index;
	uint8_t has_underflow;
	uint8_t has_overflow;
} LTTNG_PACKED;

#define LTTNG_UST_COUNTER_CONF_PADDING1	67
struct lttng_ust_counter_conf {
	uint32_t arithmetic;	/* enum lttng_ust_counter_arithmetic */
	uint32_t bitness;	/* enum lttng_ust_counter_bitness */
	uint32_t number_dimensions;
	int64_t global_sum_step;
	struct lttng_ust_counter_dimension dimensions[LTTNG_UST_COUNTER_DIMENSION_MAX];
	uint8_t coalesce_hits;
	char padding[LTTNG_UST_COUNTER_CONF_PADDING1];
} LTTNG_PACKED;

struct lttng_ust_counter_value {
	uint32_t number_dimensions;
	uint64_t dimension_indexes[LTTNG_UST_COUNTER_DIMENSION_MAX];
	int64_t value;
} LTTNG_PACKED;

#define LTTNG_UST_COUNTER_PADDING1	(LTTNG_UST_SYM_NAME_LEN + 32)
#define LTTNG_UST_COUNTER_DATA_MAX_LEN	4096U
struct lttng_ust_counter {
	uint64_t len;
	char padding[LTTNG_UST_COUNTER_PADDING1];
	char data[];	/* variable sized data */
} LTTNG_PACKED;

#define LTTNG_UST_COUNTER_GLOBAL_PADDING1	(LTTNG_UST_SYM_NAME_LEN + 32)
struct lttng_ust_counter_global {
	uint64_t len;		/* shm len */
	char padding[LTTNG_UST_COUNTER_GLOBAL_PADDING1];
} LTTNG_PACKED;

#define LTTNG_UST_COUNTER_CPU_PADDING1	(LTTNG_UST_SYM_NAME_LEN + 32)
struct lttng_ust_counter_cpu {
	uint64_t len;		/* shm len */
	uint32_t cpu_nr;
	char padding[LTTNG_UST_COUNTER_CPU_PADDING1];
} LTTNG_PACKED;

enum lttng_ust_field_type {
	LTTNG_UST_FIELD_OTHER			= 0,
	LTTNG_UST_FIELD_INTEGER			= 1,
	LTTNG_UST_FIELD_ENUM			= 2,
	LTTNG_UST_FIELD_FLOAT			= 3,
	LTTNG_UST_FIELD_STRING			= 4,
};

#define LTTNG_UST_FIELD_ITER_PADDING	(LTTNG_UST_SYM_NAME_LEN + 28)
struct lttng_ust_field_iter {
	char event_name[LTTNG_UST_SYM_NAME_LEN];
	char field_name[LTTNG_UST_SYM_NAME_LEN];
	enum lttng_ust_field_type type;
	int loglevel;				/* event loglevel */
	int nowrite;
	char padding[LTTNG_UST_FIELD_ITER_PADDING];
} LTTNG_PACKED;

enum lttng_ust_context_type {
	LTTNG_UST_CONTEXT_VTID			= 0,
	LTTNG_UST_CONTEXT_VPID			= 1,
	LTTNG_UST_CONTEXT_PTHREAD_ID		= 2,
	LTTNG_UST_CONTEXT_PROCNAME		= 3,
	LTTNG_UST_CONTEXT_IP			= 4,
	LTTNG_UST_CONTEXT_PERF_THREAD_COUNTER	= 5,
	LTTNG_UST_CONTEXT_CPU_ID		= 6,
	LTTNG_UST_CONTEXT_APP_CONTEXT		= 7,
	LTTNG_UST_CONTEXT_CGROUP_NS		= 8,
	LTTNG_UST_CONTEXT_IPC_NS		= 9,
	LTTNG_UST_CONTEXT_MNT_NS		= 10,
	LTTNG_UST_CONTEXT_NET_NS		= 11,
	LTTNG_UST_CONTEXT_PID_NS		= 12,
	LTTNG_UST_CONTEXT_USER_NS		= 13,
	LTTNG_UST_CONTEXT_UTS_NS		= 14,
	LTTNG_UST_CONTEXT_VUID			= 15,
	LTTNG_UST_CONTEXT_VEUID			= 16,
	LTTNG_UST_CONTEXT_VSUID			= 17,
	LTTNG_UST_CONTEXT_VGID			= 18,
	LTTNG_UST_CONTEXT_VEGID			= 19,
	LTTNG_UST_CONTEXT_VSGID			= 20,
	LTTNG_UST_CONTEXT_TIME_NS		= 21,
};

struct lttng_ust_perf_counter_ctx {
	uint32_t type;
	uint64_t config;
	char name[LTTNG_UST_SYM_NAME_LEN];
} LTTNG_PACKED;

#define LTTNG_UST_CONTEXT_PADDING1	16
#define LTTNG_UST_CONTEXT_PADDING2	(LTTNG_UST_SYM_NAME_LEN + 32)
struct lttng_ust_context {
	enum lttng_ust_context_type ctx;
	char padding[LTTNG_UST_CONTEXT_PADDING1];

	union {
		struct lttng_ust_perf_counter_ctx perf_counter;
		struct {
			/* Includes trailing '\0'. */
			uint32_t provider_name_len;
			uint32_t ctx_name_len;
		} app_ctx;
		char padding[LTTNG_UST_CONTEXT_PADDING2];
	} u;
} LTTNG_PACKED;

/*
 * Tracer channel attributes.
 */
#define LTTNG_UST_CHANNEL_ATTR_PADDING	(LTTNG_UST_SYM_NAME_LEN + 32)
struct lttng_ust_channel_attr {
	uint64_t subbuf_size;			/* bytes */
	uint64_t num_subbuf;			/* power of 2 */
	int overwrite;				/* 1: overwrite, 0: discard */
	unsigned int switch_timer_interval;	/* usec */
	unsigned int read_timer_interval;	/* usec */
	enum lttng_ust_output output;		/* splice, mmap */
	union {
		struct {
			int64_t blocking_timeout;	/* Blocking timeout (usec) */
		} s;
		char padding[LTTNG_UST_CHANNEL_ATTR_PADDING];
	} u;
} LTTNG_PACKED;

#define LTTNG_UST_TRACEPOINT_ITER_PADDING	16
struct lttng_ust_tracepoint_iter {
	char name[LTTNG_UST_SYM_NAME_LEN];	/* provider:name */
	int loglevel;
	char padding[LTTNG_UST_TRACEPOINT_ITER_PADDING];
} LTTNG_PACKED;

enum lttng_ust_object_type {
	LTTNG_UST_OBJECT_TYPE_UNKNOWN = -1,
	LTTNG_UST_OBJECT_TYPE_CHANNEL = 0,
	LTTNG_UST_OBJECT_TYPE_STREAM = 1,
	LTTNG_UST_OBJECT_TYPE_EVENT = 2,
	LTTNG_UST_OBJECT_TYPE_CONTEXT = 3,
	LTTNG_UST_OBJECT_TYPE_EVENT_NOTIFIER_GROUP = 4,
	LTTNG_UST_OBJECT_TYPE_EVENT_NOTIFIER = 5,
	LTTNG_UST_OBJECT_TYPE_COUNTER = 6,
	LTTNG_UST_OBJECT_TYPE_COUNTER_GLOBAL = 7,
	LTTNG_UST_OBJECT_TYPE_COUNTER_CPU = 8,
	LTTNG_UST_OBJECT_TYPE_COUNTER_EVENT = 9,
};

#define LTTNG_UST_OBJECT_DATA_PADDING1	32
#define LTTNG_UST_OBJECT_DATA_PADDING2	(LTTNG_UST_SYM_NAME_LEN + 32)

struct lttng_ust_object_data {
	enum lttng_ust_object_type type;
	int handle;
	uint64_t size;
	char padding1[LTTNG_UST_OBJECT_DATA_PADDING1];
	union {
		struct {
			void *data;
			enum lttng_ust_chan_type type;
			int wakeup_fd;
		} channel;
		struct {
			int shm_fd;
			int wakeup_fd;
			uint32_t stream_nr;
		} stream;
		struct {
			void *data;
		} counter;
		struct {
			int shm_fd;
		} counter_global;
		struct {
			int shm_fd;
			uint32_t cpu_nr;
		} counter_cpu;
		char padding2[LTTNG_UST_OBJECT_DATA_PADDING2];
	} u;
} LTTNG_PACKED;

enum lttng_ust_calibrate_type {
	LTTNG_UST_CALIBRATE_TRACEPOINT,
};

#define LTTNG_UST_CALIBRATE_PADDING1	16
#define LTTNG_UST_CALIBRATE_PADDING2	(LTTNG_UST_SYM_NAME_LEN + 32)
struct lttng_ust_calibrate {
	enum lttng_ust_calibrate_type type;	/* type (input) */
	char padding[LTTNG_UST_CALIBRATE_PADDING1];

	union {
		char padding[LTTNG_UST_CALIBRATE_PADDING2];
	} u;
} LTTNG_PACKED;

#define FILTER_BYTECODE_MAX_LEN		65536
#define LTTNG_UST_FILTER_PADDING	32
struct lttng_ust_filter_bytecode {
	uint32_t len;
	uint32_t reloc_offset;
	uint64_t seqnum;
	char padding[LTTNG_UST_FILTER_PADDING];
	char data[0];
} LTTNG_PACKED;

#define CAPTURE_BYTECODE_MAX_LEN	65536
#define LTTNG_UST_CAPTURE_PADDING	32
struct lttng_ust_capture_bytecode {
	uint32_t len;
	uint32_t reloc_offset;
	uint64_t seqnum;
	char padding[LTTNG_UST_CAPTURE_PADDING];
	char data[0];
} LTTNG_PACKED;

#define LTTNG_UST_EXCLUSION_PADDING	32
struct lttng_ust_event_exclusion {
	uint32_t count;
	char padding[LTTNG_UST_EXCLUSION_PADDING];
	char names[LTTNG_UST_SYM_NAME_LEN][0];
} LTTNG_PACKED;

#define _UST_CMD(minor)				(minor)
#define _UST_CMDR(minor, type)			(minor)
#define _UST_CMDW(minor, type)			(minor)

/* Handled by object descriptor */
#define LTTNG_UST_RELEASE			_UST_CMD(0x1)

/* Handled by object cmd */

/* LTTng-UST commands */
#define LTTNG_UST_SESSION			_UST_CMD(0x40)
#define LTTNG_UST_TRACER_VERSION		\
	_UST_CMDR(0x41, struct lttng_ust_tracer_version)
#define LTTNG_UST_TRACEPOINT_LIST		_UST_CMD(0x42)
#define LTTNG_UST_WAIT_QUIESCENT		_UST_CMD(0x43)
#define LTTNG_UST_REGISTER_DONE			_UST_CMD(0x44)
#define LTTNG_UST_TRACEPOINT_FIELD_LIST		_UST_CMD(0x45)
#define LTTNG_UST_EVENT_NOTIFIER_GROUP_CREATE	_UST_CMD(0x46)

/* Session commands */
#define LTTNG_UST_CHANNEL			\
	_UST_CMDW(0x51, struct lttng_ust_channel)
#define LTTNG_UST_SESSION_START			_UST_CMD(0x52)
#define LTTNG_UST_SESSION_STOP			_UST_CMD(0x53)
#define LTTNG_UST_SESSION_STATEDUMP		_UST_CMD(0x54)

/* Channel commands */
#define LTTNG_UST_STREAM			_UST_CMD(0x60)
#define LTTNG_UST_EVENT			\
	_UST_CMDW(0x61, struct lttng_ust_event)

/* Event and channel commands */
#define LTTNG_UST_CONTEXT			\
	_UST_CMDW(0x70, struct lttng_ust_context)
#define LTTNG_UST_FLUSH_BUFFER			\
	_UST_CMD(0x71)

/* Event, event notifier, channel and session commands */
#define LTTNG_UST_ENABLE			_UST_CMD(0x80)
#define LTTNG_UST_DISABLE			_UST_CMD(0x81)

/* Tracepoint list commands */
#define LTTNG_UST_TRACEPOINT_LIST_GET		_UST_CMD(0x90)
#define LTTNG_UST_TRACEPOINT_FIELD_LIST_GET	_UST_CMD(0x91)

/* Event and event notifier commands */
#define LTTNG_UST_FILTER			_UST_CMD(0xA0)
#define LTTNG_UST_EXCLUSION			_UST_CMD(0xA1)

/* Event notifier group commands */
#define LTTNG_UST_EVENT_NOTIFIER_CREATE		\
	_UST_CMDW(0xB0, struct lttng_ust_event_notifier)

/* Event notifier commands */
#define LTTNG_UST_CAPTURE			_UST_CMD(0xB6)

/* Session and event notifier group commands */
#define LTTNG_UST_COUNTER			\
	_UST_CMDW(0xC0, struct lttng_ust_counter)

/* Counter commands */
#define LTTNG_UST_COUNTER_GLOBAL		\
	_UST_CMDW(0xD0, struct lttng_ust_counter_global)
#define LTTNG_UST_COUNTER_CPU			\
	_UST_CMDW(0xD1, struct lttng_ust_counter_cpu)
#define LTTNG_UST_COUNTER_EVENT			\
	_UST_CMDW(0xD2, struct lttng_ust_counter_event)

#define LTTNG_UST_ROOT_HANDLE	0

struct lttng_ust_obj;

union ust_args {
	struct {
		void *chan_data;
		int wakeup_fd;
	} channel;
	struct {
		int shm_fd;
		int wakeup_fd;
	} stream;
	struct {
		struct lttng_ust_field_iter entry;
	} field_list;
	struct {
		char *ctxname;
	} app_context;
	struct {
		int event_notifier_notif_fd;
	} event_notifier_handle;
	struct {
		void *counter_data;
	} counter;
	struct {
		int shm_fd;
	} counter_shm;
};

struct lttng_ust_objd_ops {
	long (*cmd)(int objd, unsigned int cmd, unsigned long arg,
		union ust_args *args, void *owner);
	int (*release)(int objd);
};

/* Create root handle. Always ID 0. */
int lttng_abi_create_root_handle(void);

const struct lttng_ust_objd_ops *objd_ops(int id);
int lttng_ust_objd_unref(int id, int is_owner);

void lttng_ust_abi_exit(void);
void lttng_ust_events_exit(void);
void lttng_ust_objd_table_owner_cleanup(void *owner);

#endif /* _LTTNG_UST_ABI_H */
