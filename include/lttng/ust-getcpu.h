/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2014 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef LTTNG_UST_GETCPU_H
#define LTTNG_UST_GETCPU_H

#include <stdint.h>
#include <stddef.h>
#include <lttng/ust-config.h>

/* Custom upgrade 2.12 to 2.13 */

#ifndef LTTNG_UST_CUSTOM_UPGRADE_CONFLICTING_SYMBOLS
#define lttng_ust_getcpu_override	lttng_ust_getcpu_override1
#endif

/*
 * Set getcpu override read callback. This callback should return the
 * current CPU number.
 */
int lttng_ust_getcpu_override(int (*getcpu)(void));

#endif /* LTTNG_UST_GETCPU_H */
