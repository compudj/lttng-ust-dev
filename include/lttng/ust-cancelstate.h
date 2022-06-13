/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2021 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_UST_UST_CANCELSTATE_H
#define _LTTNG_UST_UST_CANCELSTATE_H

#include <lttng/ust-config.h>

/* Custom upgrade 2.12 to 2.13 */

#ifndef LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS
#define lttng_ust_cancelstate_disable_push	lttng_ust_cancelstate_disable_push1
#define lttng_ust_cancelstate_disable_pop	lttng_ust_cancelstate_disable_pop1
#endif

int lttng_ust_cancelstate_disable_push(void);
int lttng_ust_cancelstate_disable_pop(void);

#endif
