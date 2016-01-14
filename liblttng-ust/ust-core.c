/*
 * ust-core.c
 *
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdlib.h>
#include <lttng/ust-events.h>
#include <usterr-signal-safe.h>
#include "jhash.h"

static CDS_LIST_HEAD(lttng_transport_list);

struct lttng_transport *lttng_transport_find(const char *name)
{
	struct lttng_transport *transport;

	cds_list_for_each_entry(transport, &lttng_transport_list, node) {
		if (!strcmp(transport->name, name))
			return transport;
	}
	return NULL;
}

/**
 * lttng_transport_register - LTT transport registration
 * @transport: transport structure
 *
 * Registers a transport which can be used as output to extract the data out of
 * LTTng. Called with ust_lock held.
 */
void lttng_transport_register(struct lttng_transport *transport)
{
	cds_list_add_tail(&transport->node, &lttng_transport_list);
}

/**
 * lttng_transport_unregister - LTT transport unregistration
 * @transport: transport structure
 * Called with ust_lock held.
 */
void lttng_transport_unregister(struct lttng_transport *transport)
{
	cds_list_del(&transport->node);
}

/*
 * Needed by comm layer.
 */
struct lttng_enum *lttng_ust_enum_get(struct lttng_session *session,
		const char *enum_name)
{
	struct lttng_enum *_enum;
	struct cds_hlist_head *head;
	struct cds_hlist_node *node;
	size_t name_len = strlen(enum_name);
	uint32_t hash;

	hash = jhash(enum_name, name_len, 0);
	head = &session->enums_ht.table[hash & (LTTNG_UST_ENUM_HT_SIZE - 1)];
	cds_hlist_for_each_entry(_enum, node, head, hlist) {
		assert(_enum->desc);
		if (!strncmp(_enum->desc->name, enum_name,
				LTTNG_UST_SYM_NAME_LEN - 1))
			return _enum;
	}
	return NULL;
}
