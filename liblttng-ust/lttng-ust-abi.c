/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2010-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * LTTng UST ABI
 *
 * Mimic system calls for:
 * - session creation, returns an object descriptor or failure.
 *   - channel creation, returns an object descriptor or failure.
 *     - Operates on a session object descriptor
 *     - Takes all channel options as parameters.
 *   - stream get, returns an object descriptor or failure.
 *     - Operates on a channel object descriptor.
 *   - stream notifier get, returns an object descriptor or failure.
 *     - Operates on a channel object descriptor.
 *   - event creation, returns an object descriptor or failure.
 *     - Operates on a channel object descriptor
 *     - Takes an event name as parameter
 *     - Takes an instrumentation source as parameter
 *       - e.g. tracepoints, dynamic_probes...
 *     - Takes instrumentation source specific arguments.
 */

#define _LGPL_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include <urcu/compiler.h>
#include <urcu/list.h>

#include <helper.h>
#include <lttng/tracepoint.h>
#include <lttng/ust-abi.h>
#include <lttng/ust-error.h>
#include <lttng/ust-events.h>
#include <lttng/ust-version.h>
#include <ust-fd.h>
#include <usterr-signal-safe.h>

#include "../libringbuffer/frontend_types.h"
#include "../libringbuffer/shm.h"
#include "../libcounter/counter.h"
#include "tracepoint-internal.h"
#include "lttng-tracer.h"
#include "string-utils.h"
#include "ust-events-internal.h"

#define OBJ_NAME_LEN	16

static int lttng_ust_abi_close_in_progress;

static
int lttng_abi_tracepoint_list(void *owner);
static
int lttng_abi_tracepoint_field_list(void *owner);

/*
 * Object descriptor table. Should be protected from concurrent access
 * by the caller.
 */

struct lttng_ust_obj {
	union {
		struct {
			void *private_data;
			const struct lttng_ust_objd_ops *ops;
			int f_count;
			int owner_ref;	/* has ref from owner */
			void *owner;
			char name[OBJ_NAME_LEN];
		} s;
		int freelist_next;	/* offset freelist. end is -1. */
	} u;
};

struct lttng_ust_objd_table {
	struct lttng_ust_obj *array;
	unsigned int len, allocated_len;
	int freelist_head;		/* offset freelist head. end is -1 */
};

static struct lttng_ust_objd_table objd_table = {
	.freelist_head = -1,
};

static
int objd_alloc(void *private_data, const struct lttng_ust_objd_ops *ops,
		void *owner, const char *name)
{
	struct lttng_ust_obj *obj;

	if (objd_table.freelist_head != -1) {
		obj = &objd_table.array[objd_table.freelist_head];
		objd_table.freelist_head = obj->u.freelist_next;
		goto end;
	}

	if (objd_table.len >= objd_table.allocated_len) {
		unsigned int new_allocated_len, old_allocated_len;
		struct lttng_ust_obj *new_table, *old_table;

		old_allocated_len = objd_table.allocated_len;
		old_table = objd_table.array;
		if (!old_allocated_len)
			new_allocated_len = 1;
		else
			new_allocated_len = old_allocated_len << 1;
		new_table = zmalloc(sizeof(struct lttng_ust_obj) * new_allocated_len);
		if (!new_table)
			return -ENOMEM;
		memcpy(new_table, old_table,
		       sizeof(struct lttng_ust_obj) * old_allocated_len);
		free(old_table);
		objd_table.array = new_table;
		objd_table.allocated_len = new_allocated_len;
	}
	obj = &objd_table.array[objd_table.len];
	objd_table.len++;
end:
	obj->u.s.private_data = private_data;
	obj->u.s.ops = ops;
	obj->u.s.f_count = 2;	/* count == 1 : object is allocated */
				/* count == 2 : allocated + hold ref */
	obj->u.s.owner_ref = 1;	/* One owner reference */
	obj->u.s.owner = owner;
	strncpy(obj->u.s.name, name, OBJ_NAME_LEN);
	obj->u.s.name[OBJ_NAME_LEN - 1] = '\0';
	return obj - objd_table.array;
}

static
struct lttng_ust_obj *_objd_get(int id)
{
	if (id >= objd_table.len)
		return NULL;
	if (!objd_table.array[id].u.s.f_count)
		return NULL;
	return &objd_table.array[id];
}

static
void *objd_private(int id)
{
	struct lttng_ust_obj *obj = _objd_get(id);
	assert(obj);
	return obj->u.s.private_data;
}

static
void objd_set_private(int id, void *private_data)
{
	struct lttng_ust_obj *obj = _objd_get(id);
	assert(obj);
	obj->u.s.private_data = private_data;
}

const struct lttng_ust_objd_ops *objd_ops(int id)
{
	struct lttng_ust_obj *obj = _objd_get(id);

	if (!obj)
		return NULL;
	return obj->u.s.ops;
}

static
void objd_free(int id)
{
	struct lttng_ust_obj *obj = _objd_get(id);

	assert(obj);
	obj->u.freelist_next = objd_table.freelist_head;
	objd_table.freelist_head = obj - objd_table.array;
	assert(obj->u.s.f_count == 1);
	obj->u.s.f_count = 0;	/* deallocated */
}

static
void objd_ref(int id)
{
	struct lttng_ust_obj *obj = _objd_get(id);
	assert(obj != NULL);
	obj->u.s.f_count++;
}

int lttng_ust_objd_unref(int id, int is_owner)
{
	struct lttng_ust_obj *obj = _objd_get(id);

	if (!obj)
		return -EINVAL;
	if (obj->u.s.f_count == 1) {
		ERR("Reference counting error\n");
		return -EINVAL;
	}
	if (is_owner) {
		if (!obj->u.s.owner_ref) {
			ERR("Error decrementing owner reference\n");
			return -EINVAL;
		}
		obj->u.s.owner_ref--;
	}
	if ((--obj->u.s.f_count) == 1) {
		const struct lttng_ust_objd_ops *ops = objd_ops(id);

		if (ops->release)
			ops->release(id);
		objd_free(id);
	}
	return 0;
}

static
void objd_table_destroy(void)
{
	int i;

	for (i = 0; i < objd_table.allocated_len; i++) {
		struct lttng_ust_obj *obj;

		obj = _objd_get(i);
		if (!obj)
			continue;
		if (!obj->u.s.owner_ref)
			continue;	/* only unref owner ref. */
		(void) lttng_ust_objd_unref(i, 1);
	}
	free(objd_table.array);
	objd_table.array = NULL;
	objd_table.len = 0;
	objd_table.allocated_len = 0;
	objd_table.freelist_head = -1;
}

const char *lttng_ust_obj_get_name(int id)
{
	struct lttng_ust_obj *obj = _objd_get(id);

	if (!obj)
		return NULL;
	return obj->u.s.name;
}

void lttng_ust_objd_table_owner_cleanup(void *owner)
{
	int i;

	for (i = 0; i < objd_table.allocated_len; i++) {
		struct lttng_ust_obj *obj;

		obj = _objd_get(i);
		if (!obj)
			continue;
		if (!obj->u.s.owner)
			continue;	/* skip root handles */
		if (!obj->u.s.owner_ref)
			continue;	/* only unref owner ref. */
		if (obj->u.s.owner == owner)
			(void) lttng_ust_objd_unref(i, 1);
	}
}

/*
 * This is LTTng's own personal way to create an ABI for sessiond.
 * We send commands over a socket.
 */

static const struct lttng_ust_objd_ops lttng_ops;
static const struct lttng_ust_objd_ops lttng_event_notifier_group_ops;
static const struct lttng_ust_objd_ops lttng_session_ops;
static const struct lttng_ust_objd_ops lttng_channel_ops;
static const struct lttng_ust_objd_ops lttng_counter_ops;
static const struct lttng_ust_objd_ops lttng_event_enabler_ops;
static const struct lttng_ust_objd_ops lttng_event_notifier_enabler_ops;
static const struct lttng_ust_objd_ops lttng_tracepoint_list_ops;
static const struct lttng_ust_objd_ops lttng_tracepoint_field_list_ops;

int lttng_abi_create_root_handle(void)
{
	int root_handle;

	/* root handles have NULL owners */
	root_handle = objd_alloc(NULL, &lttng_ops, NULL, "root");
	return root_handle;
}

static
int lttng_is_channel_ready(struct lttng_channel *lttng_chan)
{
	struct channel *chan;
	unsigned int nr_streams, exp_streams;

	chan = lttng_chan->chan;
	nr_streams = channel_handle_get_nr_streams(lttng_chan->handle);
	exp_streams = chan->nr_streams;
	return nr_streams == exp_streams;
}

static
int lttng_abi_create_session(void *owner)
{
	struct lttng_session *session;
	int session_objd, ret;

	session = lttng_session_create();
	if (!session)
		return -ENOMEM;
	session_objd = objd_alloc(session, &lttng_session_ops, owner, "session");
	if (session_objd < 0) {
		ret = session_objd;
		goto objd_error;
	}
	session->objd = session_objd;
	session->owner = owner;
	return session_objd;

objd_error:
	lttng_session_destroy(session);
	return ret;
}

static
long lttng_abi_tracer_version(int objd,
	struct lttng_ust_tracer_version *v)
{
	v->major = LTTNG_UST_MAJOR_VERSION;
	v->minor = LTTNG_UST_MINOR_VERSION;
	v->patchlevel = LTTNG_UST_PATCHLEVEL_VERSION;
	return 0;
}

static
int lttng_abi_event_notifier_send_fd(void *owner, int *event_notifier_notif_fd)
{
	struct lttng_event_notifier_group *event_notifier_group;
	int event_notifier_group_objd, ret, fd_flag;

	event_notifier_group = lttng_event_notifier_group_create();
	if (!event_notifier_group)
		return -ENOMEM;

	/*
	 * Set this file descriptor as NON-BLOCKING.
	 */
	fd_flag = fcntl(*event_notifier_notif_fd, F_GETFL);

	fd_flag |= O_NONBLOCK;

	ret = fcntl(*event_notifier_notif_fd, F_SETFL, fd_flag);
	if (ret) {
		ret = -errno;
		goto fd_error;
	}

	event_notifier_group_objd = objd_alloc(event_notifier_group,
		&lttng_event_notifier_group_ops, owner, "event_notifier_group");
	if (event_notifier_group_objd < 0) {
		ret = event_notifier_group_objd;
		goto objd_error;
	}

	event_notifier_group->objd = event_notifier_group_objd;
	event_notifier_group->owner = owner;
	event_notifier_group->notification_fd = *event_notifier_notif_fd;
	/* Object descriptor takes ownership of notification fd. */
	*event_notifier_notif_fd = -1;

	return event_notifier_group_objd;

objd_error:
	lttng_event_notifier_group_destroy(event_notifier_group);
fd_error:
	return ret;
}

static
long lttng_abi_add_context(int objd,
	struct lttng_ust_context *context_param,
	union ust_args *uargs,
	struct lttng_ctx **ctx, struct lttng_session *session)
{
	return lttng_attach_context(context_param, uargs, ctx, session);
}

/**
 *	lttng_cmd - lttng control through socket commands
 *
 *	@objd: the object descriptor
 *	@cmd: the command
 *	@arg: command arg
 *	@uargs: UST arguments (internal)
 *	@owner: objd owner
 *
 *	This descriptor implements lttng commands:
 *	LTTNG_UST_SESSION
 *		Returns a LTTng trace session object descriptor
 *	LTTNG_UST_TRACER_VERSION
 *		Returns the LTTng kernel tracer version
 *	LTTNG_UST_TRACEPOINT_LIST
 *		Returns a file descriptor listing available tracepoints
 *	LTTNG_UST_TRACEPOINT_FIELD_LIST
 *		Returns a file descriptor listing available tracepoint fields
 *	LTTNG_UST_WAIT_QUIESCENT
 *		Returns after all previously running probes have completed
 *
 * The returned session will be deleted when its file descriptor is closed.
 */
static
long lttng_cmd(int objd, unsigned int cmd, unsigned long arg,
	union ust_args *uargs, void *owner)
{
	switch (cmd) {
	case LTTNG_UST_SESSION:
		return lttng_abi_create_session(owner);
	case LTTNG_UST_TRACER_VERSION:
		return lttng_abi_tracer_version(objd,
				(struct lttng_ust_tracer_version *) arg);
	case LTTNG_UST_TRACEPOINT_LIST:
		return lttng_abi_tracepoint_list(owner);
	case LTTNG_UST_TRACEPOINT_FIELD_LIST:
		return lttng_abi_tracepoint_field_list(owner);
	case LTTNG_UST_WAIT_QUIESCENT:
		lttng_ust_synchronize_trace();
		return 0;
	case LTTNG_UST_EVENT_NOTIFIER_GROUP_CREATE:
		return lttng_abi_event_notifier_send_fd(owner,
			&uargs->event_notifier_handle.event_notifier_notif_fd);
	default:
		return -EINVAL;
	}
}

static const struct lttng_ust_objd_ops lttng_ops = {
	.cmd = lttng_cmd,
};

static
int lttng_abi_map_channel(int session_objd,
		struct lttng_ust_channel *ust_chan,
		union ust_args *uargs,
		void *owner)
{
	struct lttng_session *session = objd_private(session_objd);
	const char *transport_name;
	const struct lttng_transport *transport;
	const char *chan_name;
	int chan_objd;
	struct lttng_ust_shm_handle *channel_handle;
	struct lttng_channel *lttng_chan;
	struct channel *chan;
	struct lttng_ust_lib_ring_buffer_config *config;
	void *chan_data;
	int wakeup_fd;
	uint64_t len;
	int ret;
	enum lttng_ust_chan_type type;

	chan_data = uargs->channel.chan_data;
	wakeup_fd = uargs->channel.wakeup_fd;
	len = ust_chan->len;
	type = ust_chan->type;

	switch (type) {
	case LTTNG_UST_CHAN_PER_CPU:
		break;
	default:
		ret = -EINVAL;
		goto invalid;
	}

	if (session->been_active) {
		ret = -EBUSY;
		goto active;	/* Refuse to add channel to active session */
	}

	channel_handle = channel_handle_create(chan_data, len, wakeup_fd);
	if (!channel_handle) {
		ret = -EINVAL;
		goto handle_error;
	}

	/* Ownership of chan_data and wakeup_fd taken by channel handle. */
	uargs->channel.chan_data = NULL;
	uargs->channel.wakeup_fd = -1;

	chan = shmp(channel_handle, channel_handle->chan);
	assert(chan);
	chan->handle = channel_handle;
	config = &chan->backend.config;
	lttng_chan = channel_get_private(chan);
	if (!lttng_chan) {
		ret = -EINVAL;
		goto alloc_error;
	}

	/* Lookup transport name */
	switch (type) {
	case LTTNG_UST_CHAN_PER_CPU:
		if (config->output == RING_BUFFER_MMAP) {
			if (config->mode == RING_BUFFER_OVERWRITE) {
				if (config->wakeup == RING_BUFFER_WAKEUP_BY_WRITER) {
					transport_name = "relay-overwrite-mmap";
				} else {
					transport_name = "relay-overwrite-rt-mmap";
				}
			} else {
				if (config->wakeup == RING_BUFFER_WAKEUP_BY_WRITER) {
					transport_name = "relay-discard-mmap";
				} else {
					transport_name = "relay-discard-rt-mmap";
				}
			}
		} else {
			ret = -EINVAL;
			goto notransport;
		}
		chan_name = "channel";
		break;
	default:
		ret = -EINVAL;
		goto notransport;
	}
	transport = lttng_transport_find(transport_name);
	if (!transport) {
		DBG("LTTng transport %s not found\n",
		       transport_name);
		ret = -EINVAL;
		goto notransport;
	}

	chan_objd = objd_alloc(NULL, &lttng_channel_ops, owner, chan_name);
	if (chan_objd < 0) {
		ret = chan_objd;
		goto objd_error;
	}

	/* Initialize our lttng chan */
	lttng_chan->chan = chan;
	lttng_chan->enabled = 1;	/* Backward compatibility */
	lttng_chan->ctx = NULL;
	lttng_chan->session = session;	/* Backward compatibility */
	lttng_chan->ops = &transport->ops;
	memcpy(&lttng_chan->chan->backend.config,
		transport->client_config,
		sizeof(lttng_chan->chan->backend.config));
	cds_list_add(&lttng_chan->node, &session->chan_head);
	lttng_chan->header_type = 0;
	lttng_chan->handle = channel_handle;
	lttng_chan->type = type;

	/* Set container properties */
	lttng_chan->parent.type = LTTNG_EVENT_CONTAINER_CHANNEL;
	lttng_chan->parent.session = session;
	lttng_chan->parent.enabled = 1;
	lttng_chan->parent.tstate = 1;
	/*
	 * The ring buffer always coalesces hits from various event
	 * enablers matching a given event to a single event record within the
	 * ring buffer.
	 */
	lttng_chan->parent.coalesce_hits = true;

	/*
	 * We tolerate no failure path after channel creation. It will stay
	 * invariant for the rest of the session.
	 */
	objd_set_private(chan_objd, lttng_chan);
	lttng_chan->parent.objd = chan_objd;
	/* The channel created holds a reference on the session */
	objd_ref(session_objd);
	return chan_objd;

	/* error path after channel was created */
objd_error:
notransport:
alloc_error:
	channel_destroy(chan, channel_handle, 0);
	return ret;

handle_error:
active:
invalid:
	return ret;
}

static
long lttng_abi_session_create_counter(
		int session_objd,
		struct lttng_ust_counter *ust_counter,
		union ust_args *uargs,
		void *owner)
{
	struct lttng_session *session = objd_private(session_objd);
	int counter_objd, ret, i;
	char *counter_transport_name;
	struct lttng_counter *counter = NULL;
	struct lttng_event_container *container;
	size_t dimension_sizes[LTTNG_UST_COUNTER_DIMENSION_MAX] = { 0 };
	size_t number_dimensions;
	const struct lttng_ust_counter_conf *counter_conf;

	if (ust_counter->len != sizeof(*counter_conf)) {
		ERR("LTTng: Map: Error counter configuration of wrong size.\n");
		return -EINVAL;
	}
	counter_conf = uargs->counter.counter_data;

	if (counter_conf->arithmetic != LTTNG_UST_COUNTER_ARITHMETIC_MODULAR) {
		ERR("LTTng: Map: Error counter of the wrong type.\n");
		return -EINVAL;
	}

	if (counter_conf->global_sum_step) {
		/* Unsupported. */
		return -EINVAL;
	}

	switch (counter_conf->bitness) {
	case LTTNG_UST_COUNTER_BITNESS_64:
		counter_transport_name = "counter-per-cpu-64-modular";
		break;
	case LTTNG_UST_COUNTER_BITNESS_32:
		counter_transport_name = "counter-per-cpu-32-modular";
		break;
	default:
		return -EINVAL;
	}

	number_dimensions = (size_t) counter_conf->number_dimensions;

	for (i = 0; i < counter_conf->number_dimensions; i++) {
		if (counter_conf->dimensions[i].has_underflow)
			return -EINVAL;
		if (counter_conf->dimensions[i].has_overflow)
			return -EINVAL;
		dimension_sizes[i] = counter_conf->dimensions[i].size;
	}

	counter_objd = objd_alloc(NULL, &lttng_counter_ops, owner, "counter");
	if (counter_objd < 0) {
		ret = counter_objd;
		goto objd_error;
	}

	counter = lttng_session_create_counter(session,
			counter_transport_name,
			number_dimensions, dimension_sizes,
			0, counter_conf->coalesce_hits);
	if (!counter) {
		ret = -EINVAL;
		goto counter_error;
	}
	container = lttng_counter_get_event_container(counter);
	objd_set_private(counter_objd, counter);
	container->objd = counter_objd;
	container->enabled = 1;
	container->tstate = 1;
	/* The channel created holds a reference on the session */
	objd_ref(session_objd);
	return counter_objd;

counter_error:
	{
		int err;

		err = lttng_ust_objd_unref(counter_objd, 1);
		assert(!err);
	}
objd_error:
	return ret;
}


/**
 *	lttng_session_cmd - lttng session object command
 *
 *	@obj: the object
 *	@cmd: the command
 *	@arg: command arg
 *	@uargs: UST arguments (internal)
 *	@owner: objd owner
 *
 *	This descriptor implements lttng commands:
 *	LTTNG_UST_CHANNEL
 *		Returns a LTTng channel object descriptor
 *	LTTNG_UST_ENABLE
 *		Enables tracing for a session (weak enable)
 *	LTTNG_UST_DISABLE
 *		Disables tracing for a session (strong disable)
 *
 * The returned channel will be deleted when its file descriptor is closed.
 */
static
long lttng_session_cmd(int objd, unsigned int cmd, unsigned long arg,
	union ust_args *uargs, void *owner)
{
	struct lttng_session *session = objd_private(objd);

	switch (cmd) {
	case LTTNG_UST_CHANNEL:
		return lttng_abi_map_channel(objd,
				(struct lttng_ust_channel *) arg,
				uargs, owner);
	case LTTNG_UST_SESSION_START:
	case LTTNG_UST_ENABLE:
		return lttng_session_enable(session);
	case LTTNG_UST_SESSION_STOP:
	case LTTNG_UST_DISABLE:
		return lttng_session_disable(session);
	case LTTNG_UST_SESSION_STATEDUMP:
		return lttng_session_statedump(session);
	case LTTNG_UST_COUNTER:
		return lttng_abi_session_create_counter(objd,
			(struct lttng_ust_counter *) arg,
			uargs, owner);
	case LTTNG_UST_COUNTER_GLOBAL:
	case LTTNG_UST_COUNTER_CPU:
		/* Not implemented yet. */
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

/*
 * Called when the last file reference is dropped.
 *
 * Big fat note: channels and events are invariant for the whole session after
 * their creation. So this session destruction also destroys all channel and
 * event structures specific to this session (they are not destroyed when their
 * individual file is released).
 */
static
int lttng_release_session(int objd)
{
	struct lttng_session *session = objd_private(objd);

	if (session) {
		lttng_session_destroy(session);
		return 0;
	} else {
		return -EINVAL;
	}
}

static const struct lttng_ust_objd_ops lttng_session_ops = {
	.release = lttng_release_session,
	.cmd = lttng_session_cmd,
};

static int lttng_ust_event_notifier_enabler_create(int event_notifier_group_obj,
		void *owner, struct lttng_ust_event_notifier *event_notifier_param,
		enum lttng_enabler_format_type type)
{
	struct lttng_event_notifier_group *event_notifier_group =
		objd_private(event_notifier_group_obj);
	struct lttng_event_notifier_enabler *event_notifier_enabler;
	int event_notifier_objd, ret;

	event_notifier_param->event.name[LTTNG_UST_SYM_NAME_LEN - 1] = '\0';
	event_notifier_objd = objd_alloc(NULL, &lttng_event_notifier_enabler_ops, owner,
		"event_notifier enabler");
	if (event_notifier_objd < 0) {
		ret = event_notifier_objd;
		goto objd_error;
	}

	event_notifier_enabler = lttng_event_notifier_enabler_create(
		event_notifier_group, type, event_notifier_param);
	if (!event_notifier_enabler) {
		ret = -ENOMEM;
		goto event_notifier_error;
	}

	objd_set_private(event_notifier_objd, event_notifier_enabler);
	/* The event_notifier holds a reference on the event_notifier group. */
	objd_ref(event_notifier_enabler->group->objd);

	return event_notifier_objd;

event_notifier_error:
	{
		int err;

		err = lttng_ust_objd_unref(event_notifier_objd, 1);
		assert(!err);
	}
objd_error:
	return ret;
}

static
long lttng_event_notifier_enabler_cmd(int objd, unsigned int cmd, unsigned long arg,
		union ust_args *uargs, void *owner)
{
	struct lttng_event_notifier_enabler *event_notifier_enabler = objd_private(objd);
	switch (cmd) {
	case LTTNG_UST_FILTER:
		return lttng_event_notifier_enabler_attach_filter_bytecode(
			event_notifier_enabler,
			(struct lttng_ust_bytecode_node **) arg);
	case LTTNG_UST_EXCLUSION:
		return lttng_event_notifier_enabler_attach_exclusion(event_notifier_enabler,
			(struct lttng_ust_excluder_node **) arg);
	case LTTNG_UST_CAPTURE:
		return lttng_event_notifier_enabler_attach_capture_bytecode(
			event_notifier_enabler,
			(struct lttng_ust_bytecode_node **) arg);
	case LTTNG_UST_ENABLE:
		return lttng_event_notifier_enabler_enable(event_notifier_enabler);
	case LTTNG_UST_DISABLE:
		return lttng_event_notifier_enabler_disable(event_notifier_enabler);
	default:
		return -EINVAL;
	}
}

/**
 *	lttng_event_notifier_group_error_counter_cmd - lttng event_notifier group error counter object command
 *
 *	@obj: the object
 *	@cmd: the command
 *	@arg: command arg
 *	@uargs: UST arguments (internal)
 *	@owner: objd owner
 *
 *	This descriptor implements lttng commands:
 *      LTTNG_UST_COUNTER_GLOBAL
 *        Return negative error code on error, 0 on success.
 *      LTTNG_UST_COUNTER_CPU
 *        Return negative error code on error, 0 on success.
 */
static
long lttng_event_notifier_group_error_counter_cmd(int objd, unsigned int cmd, unsigned long arg,
	union ust_args *uargs, void *owner)
{
	int ret;
	struct lttng_counter *counter = objd_private(objd);

	switch (cmd) {
	case LTTNG_UST_COUNTER_GLOBAL:
		ret = -EINVAL;	/* Unimplemented. */
		break;
	case LTTNG_UST_COUNTER_CPU:
	{
		struct lttng_ust_counter_cpu *counter_cpu =
			(struct lttng_ust_counter_cpu *)arg;

		ret = lttng_counter_set_cpu_shm(counter->counter,
			counter_cpu->cpu_nr, uargs->counter_shm.shm_fd);
		if (!ret) {
			/* Take ownership of the shm_fd. */
			uargs->counter_shm.shm_fd = -1;
		}
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

LTTNG_HIDDEN
int lttng_release_event_notifier_group_error_counter(int objd)
{
	struct lttng_counter *counter = objd_private(objd);

	if (counter) {
		return lttng_ust_objd_unref(counter->owner.event_notifier_group->objd, 0);
	} else {
		return -EINVAL;
	}
}

static const struct lttng_ust_objd_ops lttng_event_notifier_group_error_counter_ops = {
	.release = lttng_release_event_notifier_group_error_counter,
	.cmd = lttng_event_notifier_group_error_counter_cmd,
};

static
int lttng_ust_event_notifier_group_create_error_counter(int event_notifier_group_objd, void *owner,
		struct lttng_ust_counter_conf *error_counter_conf)
{
	const char *counter_transport_name;
	struct lttng_event_notifier_group *event_notifier_group =
		objd_private(event_notifier_group_objd);
	struct lttng_counter *counter;
	struct lttng_event_container *container;
	int counter_objd, ret;
	size_t counter_len;

	if (event_notifier_group->error_counter)
		return -EBUSY;

	if (error_counter_conf->arithmetic != LTTNG_UST_COUNTER_ARITHMETIC_MODULAR)
		return -EINVAL;

	if (error_counter_conf->number_dimensions != 1)
		return -EINVAL;

	if (error_counter_conf->dimensions[0].has_underflow)
		return -EINVAL;

	if (error_counter_conf->dimensions[0].has_overflow)
		return -EINVAL;

	switch (error_counter_conf->bitness) {
	case LTTNG_UST_COUNTER_BITNESS_64:
		counter_transport_name = "counter-per-cpu-64-modular";
		break;
	case LTTNG_UST_COUNTER_BITNESS_32:
		counter_transport_name = "counter-per-cpu-32-modular";
		break;
	default:
		return -EINVAL;
	}

	counter_objd = objd_alloc(NULL, &lttng_event_notifier_group_error_counter_ops, owner,
		"event_notifier group error counter");
	if (counter_objd < 0) {
		ret = counter_objd;
		goto objd_error;
	}

	counter_len = error_counter_conf->dimensions[0].size;
	counter = lttng_ust_counter_create(counter_transport_name, 1, &counter_len, 0, false);
	if (!counter) {
		ret = -EINVAL;
		goto create_error;
	}
	container = lttng_counter_get_event_container(counter);

	event_notifier_group->error_counter_len = counter_len;
	/*
	 * store-release to publish error counter matches load-acquire
	 * in record_error. Ensures the counter is created and the
	 * error_counter_len is set before they are used.
	 * Currently a full memory barrier is used, which could be
	 * turned into acquire-release barriers.
	 */
	cmm_smp_mb();
	CMM_STORE_SHARED(event_notifier_group->error_counter, counter);

	container->objd = counter_objd;
	counter->owner.event_notifier_group = event_notifier_group;	/* owner */

	objd_set_private(counter_objd, counter);
	/* The error counter holds a reference on the event_notifier group. */
	objd_ref(event_notifier_group->objd);

	return counter_objd;

create_error:
	{
		int err;

		err = lttng_ust_objd_unref(counter_objd, 1);
		assert(!err);
	}
objd_error:
	return ret;
}

static
long lttng_event_notifier_group_cmd(int objd, unsigned int cmd, unsigned long arg,
		union ust_args *uargs, void *owner)
{
	switch (cmd) {
	case LTTNG_UST_EVENT_NOTIFIER_CREATE:
	{
		struct lttng_ust_event_notifier *event_notifier_param =
			(struct lttng_ust_event_notifier *) arg;
		if (strutils_is_star_glob_pattern(event_notifier_param->event.name)) {
			/*
			 * If the event name is a star globbing pattern,
			 * we create the special star globbing enabler.
			 */
			return lttng_ust_event_notifier_enabler_create(objd,
					owner, event_notifier_param,
					LTTNG_ENABLER_FORMAT_STAR_GLOB);
		} else {
			return lttng_ust_event_notifier_enabler_create(objd,
					owner, event_notifier_param,
					LTTNG_ENABLER_FORMAT_EVENT);
		}
	}
	case LTTNG_UST_COUNTER:
	{
		struct lttng_ust_counter_conf *counter_conf =
			(struct lttng_ust_counter_conf *) uargs->counter.counter_data;
		return lttng_ust_event_notifier_group_create_error_counter(
				objd, owner, counter_conf);
	}
	default:
		return -EINVAL;
	}
}

static
int lttng_event_notifier_enabler_release(int objd)
{
	struct lttng_event_notifier_enabler *event_notifier_enabler = objd_private(objd);

	if (event_notifier_enabler)
		return lttng_ust_objd_unref(event_notifier_enabler->group->objd, 0);
	return 0;
}

static const struct lttng_ust_objd_ops lttng_event_notifier_enabler_ops = {
	.release = lttng_event_notifier_enabler_release,
	.cmd = lttng_event_notifier_enabler_cmd,
};

static
int lttng_release_event_notifier_group(int objd)
{
	struct lttng_event_notifier_group *event_notifier_group = objd_private(objd);

	if (event_notifier_group) {
		lttng_event_notifier_group_destroy(event_notifier_group);
		return 0;
	} else {
		return -EINVAL;
	}
}

static const struct lttng_ust_objd_ops lttng_event_notifier_group_ops = {
	.release = lttng_release_event_notifier_group,
	.cmd = lttng_event_notifier_group_cmd,
};

static
long lttng_tracepoint_list_cmd(int objd, unsigned int cmd, unsigned long arg,
	union ust_args *uargs, void *owner)
{
	struct lttng_ust_tracepoint_list *list = objd_private(objd);
	struct lttng_ust_tracepoint_iter *tp =
		(struct lttng_ust_tracepoint_iter *) arg;
	struct lttng_ust_tracepoint_iter *iter;

	switch (cmd) {
	case LTTNG_UST_TRACEPOINT_LIST_GET:
	{
		iter = lttng_ust_tracepoint_list_get_iter_next(list);
		if (!iter)
			return -LTTNG_UST_ERR_NOENT;
		memcpy(tp, iter, sizeof(*tp));
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static
int lttng_abi_tracepoint_list(void *owner)
{
	int list_objd, ret;
	struct lttng_ust_tracepoint_list *list;

	list_objd = objd_alloc(NULL, &lttng_tracepoint_list_ops, owner, "tp_list");
	if (list_objd < 0) {
		ret = list_objd;
		goto objd_error;
	}
	list = zmalloc(sizeof(*list));
	if (!list) {
		ret = -ENOMEM;
		goto alloc_error;
	}
	objd_set_private(list_objd, list);

	/* populate list by walking on all registered probes. */
	ret = lttng_probes_get_event_list(list);
	if (ret) {
		goto list_error;
	}
	return list_objd;

list_error:
	free(list);
alloc_error:
	{
		int err;

		err = lttng_ust_objd_unref(list_objd, 1);
		assert(!err);
	}
objd_error:
	return ret;
}

static
int lttng_release_tracepoint_list(int objd)
{
	struct lttng_ust_tracepoint_list *list = objd_private(objd);

	if (list) {
		lttng_probes_prune_event_list(list);
		free(list);
		return 0;
	} else {
		return -EINVAL;
	}
}

static const struct lttng_ust_objd_ops lttng_tracepoint_list_ops = {
	.release = lttng_release_tracepoint_list,
	.cmd = lttng_tracepoint_list_cmd,
};

static
long lttng_tracepoint_field_list_cmd(int objd, unsigned int cmd,
	unsigned long arg, union ust_args *uargs, void *owner)
{
	struct lttng_ust_field_list *list = objd_private(objd);
	struct lttng_ust_field_iter *tp = &uargs->field_list.entry;
	struct lttng_ust_field_iter *iter;

	switch (cmd) {
	case LTTNG_UST_TRACEPOINT_FIELD_LIST_GET:
	{
		iter = lttng_ust_field_list_get_iter_next(list);
		if (!iter)
			return -LTTNG_UST_ERR_NOENT;
		memcpy(tp, iter, sizeof(*tp));
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static
int lttng_abi_tracepoint_field_list(void *owner)
{
	int list_objd, ret;
	struct lttng_ust_field_list *list;

	list_objd = objd_alloc(NULL, &lttng_tracepoint_field_list_ops, owner,
			"tp_field_list");
	if (list_objd < 0) {
		ret = list_objd;
		goto objd_error;
	}
	list = zmalloc(sizeof(*list));
	if (!list) {
		ret = -ENOMEM;
		goto alloc_error;
	}
	objd_set_private(list_objd, list);

	/* populate list by walking on all registered probes. */
	ret = lttng_probes_get_field_list(list);
	if (ret) {
		goto list_error;
	}
	return list_objd;

list_error:
	free(list);
alloc_error:
	{
		int err;

		err = lttng_ust_objd_unref(list_objd, 1);
		assert(!err);
	}
objd_error:
	return ret;
}

static
int lttng_release_tracepoint_field_list(int objd)
{
	struct lttng_ust_field_list *list = objd_private(objd);

	if (list) {
		lttng_probes_prune_field_list(list);
		free(list);
		return 0;
	} else {
		return -EINVAL;
	}
}

static const struct lttng_ust_objd_ops lttng_tracepoint_field_list_ops = {
	.release = lttng_release_tracepoint_field_list,
	.cmd = lttng_tracepoint_field_list_cmd,
};

static
int lttng_abi_map_stream(int channel_objd, struct lttng_ust_stream *info,
		union ust_args *uargs, void *owner)
{
	struct lttng_channel *channel = objd_private(channel_objd);
	int ret;

	ret = channel_handle_add_stream(channel->handle,
		uargs->stream.shm_fd, uargs->stream.wakeup_fd,
		info->stream_nr, info->len);
	if (ret)
		goto error_add_stream;
	/* Take ownership of shm_fd and wakeup_fd. */
	uargs->stream.shm_fd = -1;
	uargs->stream.wakeup_fd = -1;

	return 0;

error_add_stream:
	return ret;
}

static
int lttng_abi_create_event_enabler(int objd,	/* channel or counter objd */
			   struct lttng_event_container *container,
			   struct lttng_ust_event *event_param,
			   struct lttng_ust_counter_key *key_param,
			   void *owner,
			   enum lttng_enabler_format_type format_type)
{
	struct lttng_event_enabler *enabler;
	int event_objd, ret;

	event_param->name[LTTNG_UST_SYM_NAME_LEN - 1] = '\0';
	event_objd = objd_alloc(NULL, &lttng_event_enabler_ops, owner,
		"event enabler");
	if (event_objd < 0) {
		ret = event_objd;
		goto objd_error;
	}
	/*
	 * We tolerate no failure path after event creation. It will stay
	 * invariant for the rest of the session.
	 */
	enabler = lttng_event_enabler_create(format_type, event_param,
			key_param, container);
	if (!enabler) {
		ret = -ENOMEM;
		goto event_error;
	}
	objd_set_private(event_objd, enabler);
	/* The event holds a reference on the channel or counter */
	objd_ref(objd);
	return event_objd;

event_error:
	{
		int err;

		err = lttng_ust_objd_unref(event_objd, 1);
		assert(!err);
	}
objd_error:
	return ret;
}

/**
 *	lttng_channel_cmd - lttng control through object descriptors
 *
 *	@objd: the object descriptor
 *	@cmd: the command
 *	@arg: command arg
 *	@uargs: UST arguments (internal)
 *	@owner: objd owner
 *
 *	This object descriptor implements lttng commands:
 *      LTTNG_UST_STREAM
 *              Returns an event stream object descriptor or failure.
 *              (typically, one event stream records events from one CPU)
 *	LTTNG_UST_EVENT
 *		Returns an event object descriptor or failure.
 *	LTTNG_UST_CONTEXT
 *		Prepend a context field to each event in the channel
 *	LTTNG_UST_ENABLE
 *		Enable recording for events in this channel (weak enable)
 *	LTTNG_UST_DISABLE
 *		Disable recording for events in this channel (strong disable)
 *
 * Channel and event file descriptors also hold a reference on the session.
 */
static
long lttng_channel_cmd(int objd, unsigned int cmd, unsigned long arg,
	union ust_args *uargs, void *owner)
{
	struct lttng_channel *channel = objd_private(objd);
	struct lttng_event_container *container = lttng_channel_get_event_container(channel);

	if (cmd != LTTNG_UST_STREAM) {
		/*
		 * Check if channel received all streams.
		 */
		if (!lttng_is_channel_ready(channel))
			return -EPERM;
	}

	switch (cmd) {
	case LTTNG_UST_STREAM:
	{
		struct lttng_ust_stream *stream;

		stream = (struct lttng_ust_stream *) arg;
		/* stream used as output */
		return lttng_abi_map_stream(objd, stream, uargs, owner);
	}
	case LTTNG_UST_EVENT:
	{
		struct lttng_ust_event *event_param =
			(struct lttng_ust_event *) arg;

		if (strutils_is_star_glob_pattern(event_param->name)) {
			/*
			 * If the event name is a star globbing pattern,
			 * we create the special star globbing enabler.
			 */
			return lttng_abi_create_event_enabler(objd, container, event_param, NULL,
					owner, LTTNG_ENABLER_FORMAT_STAR_GLOB);
		} else {
			return lttng_abi_create_event_enabler(objd, container, event_param, NULL,
					owner, LTTNG_ENABLER_FORMAT_EVENT);
		}
	}
	case LTTNG_UST_CONTEXT:
		return lttng_abi_add_context(objd,
				(struct lttng_ust_context *) arg, uargs,
				&channel->ctx, channel->session);
	case LTTNG_UST_ENABLE:
		return lttng_event_container_enable(&channel->parent);
	case LTTNG_UST_DISABLE:
		return lttng_event_container_disable(&channel->parent);
	case LTTNG_UST_FLUSH_BUFFER:
		return channel->ops->flush_buffer(channel->chan, channel->handle);
	default:
		return -EINVAL;
	}
}

static
int lttng_channel_release(int objd)
{
	struct lttng_channel *channel = objd_private(objd);

	if (channel)
		return lttng_ust_objd_unref(channel->session->objd, 0);
	return 0;
}

static const struct lttng_ust_objd_ops lttng_channel_ops = {
	.release = lttng_channel_release,
	.cmd = lttng_channel_cmd,
};

/**
 *	lttng_counter_cmd - lttng control through object descriptors
 *
 *	@objd: the object descriptor
 *	@cmd: the command
 *	@arg: command arg
 *	@uargs: UST arguments (internal)
 *	@owner: objd owner
 *
 *	This object descriptor implements lttng commands:
 *      LTTNG_UST_COUNTER_GLOBAL:
 *              Returns a global counter object descriptor or failure.
 *      LTTNG_UST_COUNTER_CPU:
 *              Returns a per-cpu counter object descriptor or failure.
 *	LTTNG_UST_COUNTER_EVENT
 *		Returns an event object descriptor or failure.
 *	LTTNG_UST_ENABLE
 *		Enable recording for events in this channel (weak enable)
 *	LTTNG_UST_DISABLE
 *		Disable recording for events in this channel (strong disable)
 *
 * Counter and event object descriptors also hold a reference on the session.
 */
static
long lttng_counter_cmd(int objd, unsigned int cmd, unsigned long arg,
	union ust_args *uargs, void *owner)
{
	struct lttng_counter *counter = objd_private(objd);
	struct lttng_event_container *container = lttng_counter_get_event_container(counter);

	if (cmd != LTTNG_UST_COUNTER_GLOBAL && cmd != LTTNG_UST_COUNTER_CPU) {
		/*
		 * Check if counter received all global/per-cpu objects.
		 */
		if (!lttng_counter_ready(counter->counter))
			return -EPERM;
	}

	switch (cmd) {
	case LTTNG_UST_COUNTER_GLOBAL:
	{
		long ret;
		int shm_fd;

		shm_fd = uargs->counter_shm.shm_fd;
		ret = lttng_counter_set_global_shm(counter->counter, shm_fd);
		if (!ret) {
			/* Take ownership of shm_fd. */
			uargs->counter_shm.shm_fd = -1;
		}
		return ret;
	}
	case LTTNG_UST_COUNTER_CPU:
	{
		struct lttng_ust_counter_cpu *counter_cpu =
			(struct lttng_ust_counter_cpu *) arg;
		long ret;
		int shm_fd;

		shm_fd = uargs->counter_shm.shm_fd;
		ret = lttng_counter_set_cpu_shm(counter->counter,
				counter_cpu->cpu_nr, shm_fd);
		if (!ret) {
			/* Take ownership of shm_fd. */
			uargs->counter_shm.shm_fd = -1;
		}
		return ret;
	}
	case LTTNG_UST_COUNTER_EVENT:
	{
		struct lttng_ust_counter_event *counter_event_param =
			(struct lttng_ust_counter_event *) arg;
		struct lttng_ust_event *event_param = &counter_event_param->event;
		struct lttng_ust_counter_key *key_param = &counter_event_param->key;

		if (strutils_is_star_glob_pattern(event_param->name)) {
			/*
			 * If the event name is a star globbing pattern,
			 * we create the special star globbing enabler.
			 */
			return lttng_abi_create_event_enabler(objd, container, event_param, key_param,
					owner, LTTNG_ENABLER_FORMAT_STAR_GLOB);
		} else {
			return lttng_abi_create_event_enabler(objd, container, event_param, key_param,
					owner, LTTNG_ENABLER_FORMAT_EVENT);
		}
	}
	case LTTNG_UST_ENABLE:
		return lttng_event_container_enable(container);
	case LTTNG_UST_DISABLE:
		return lttng_event_container_disable(container);
	default:
		return -EINVAL;
	}
}


static
int lttng_counter_release(int objd)
{
	struct lttng_counter *counter = objd_private(objd);

	if (counter) {
		struct lttng_event_container *container = lttng_counter_get_event_container(counter);

		return lttng_ust_objd_unref(container->session->objd, 0);
	}
	return 0;
}

static const struct lttng_ust_objd_ops lttng_counter_ops = {
	.release = lttng_counter_release,
	.cmd = lttng_counter_cmd,
};

/**
 *	lttng_enabler_cmd - lttng control through object descriptors
 *
 *	@objd: the object descriptor
 *	@cmd: the command
 *	@arg: command arg
 *	@uargs: UST arguments (internal)
 *	@owner: objd owner
 *
 *	This object descriptor implements lttng commands:
 *	LTTNG_UST_CONTEXT
 *		Prepend a context field to each record of events of this
 *		enabler.
 *	LTTNG_UST_ENABLE
 *		Enable recording for this enabler
 *	LTTNG_UST_DISABLE
 *		Disable recording for this enabler
 *	LTTNG_UST_FILTER
 *		Attach a filter to an enabler.
 *	LTTNG_UST_EXCLUSION
 *		Attach exclusions to an enabler.
 */
static
long lttng_event_enabler_cmd(int objd, unsigned int cmd, unsigned long arg,
	union ust_args *uargs, void *owner)
{
	struct lttng_event_enabler *enabler = objd_private(objd);

	switch (cmd) {
	case LTTNG_UST_CONTEXT:
		return lttng_event_enabler_attach_context(enabler,
				(struct lttng_ust_context *) arg);
	case LTTNG_UST_ENABLE:
		return lttng_event_enabler_enable(enabler);
	case LTTNG_UST_DISABLE:
		return lttng_event_enabler_disable(enabler);
	case LTTNG_UST_FILTER:
	{
		int ret;

		ret = lttng_event_enabler_attach_filter_bytecode(enabler,
				(struct lttng_ust_bytecode_node **) arg);
		if (ret)
			return ret;
		return 0;
	}
	case LTTNG_UST_EXCLUSION:
	{
		return lttng_event_enabler_attach_exclusion(enabler,
				(struct lttng_ust_excluder_node **) arg);
	}
	default:
		return -EINVAL;
	}
}

static
int lttng_event_enabler_release(int objd)
{
	struct lttng_event_enabler *event_enabler = objd_private(objd);

	if (event_enabler)
		return lttng_ust_objd_unref(event_enabler->container->objd, 0);

	return 0;
}

static const struct lttng_ust_objd_ops lttng_event_enabler_ops = {
	.release = lttng_event_enabler_release,
	.cmd = lttng_event_enabler_cmd,
};

void lttng_ust_abi_exit(void)
{
	lttng_ust_abi_close_in_progress = 1;
	ust_lock_nocheck();
	objd_table_destroy();
	ust_unlock();
	lttng_ust_abi_close_in_progress = 0;
}
