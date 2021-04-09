/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2017 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <usterr-signal-safe.h>
#include <ust-helper.h>
#include "getenv.h"

enum lttng_env_secure {
	LTTNG_ENV_SECURE,
	LTTNG_ENV_NOT_SECURE,
};

struct lttng_env {
	const char *key;
	enum lttng_env_secure secure;
	char *value;
};

static struct lttng_env lttng_env[] = {
	/*
	 * LTTNG_UST_DEBUG is used directly by snprintf, because it
	 * needs to be already set for ERR() used in
	 * lttng_ust_getenv_init().
	 */
	{ "LTTNG_UST_DEBUG", LTTNG_ENV_NOT_SECURE, NULL, },

	/* Env. var. which can be used in setuid/setgid executables. */
	{ "LTTNG_UST_WITHOUT_BADDR_STATEDUMP", LTTNG_ENV_NOT_SECURE, NULL, },
	{ "LTTNG_UST_REGISTER_TIMEOUT", LTTNG_ENV_NOT_SECURE, NULL, },

	/* Env. var. which are not fetched in setuid/setgid executables. */
	{ "LTTNG_UST_CLOCK_PLUGIN", LTTNG_ENV_SECURE, NULL, },
	{ "LTTNG_UST_GETCPU_PLUGIN", LTTNG_ENV_SECURE, NULL, },
	{ "LTTNG_UST_ALLOW_BLOCKING", LTTNG_ENV_SECURE, NULL, },
	{ "HOME", LTTNG_ENV_SECURE, NULL, },
	{ "LTTNG_HOME", LTTNG_ENV_SECURE, NULL, },
};

static
int lttng_is_setuid_setgid(void)
{
	return geteuid() != getuid() || getegid() != getgid();
}

char *lttng_ust_getenv(const char *name)
{
	size_t i;
	struct lttng_env *e;
	bool found = false;

	for (i = 0; i < LTTNG_ARRAY_SIZE(lttng_env); i++) {
		e = &lttng_env[i];

		if (strcmp(e->key, name) == 0) {
			found = true;
			break;
		}
	}
	if (!found) {
		return NULL;
	}
	return e->value;
}

void lttng_ust_getenv_init(void)
{
	size_t i;

	for (i = 0; i < LTTNG_ARRAY_SIZE(lttng_env); i++) {
		struct lttng_env *e = &lttng_env[i];

		if (e->secure == LTTNG_ENV_SECURE && lttng_is_setuid_setgid()) {
			ERR("Getting environment variable '%s' from setuid/setgid binary refused for security reasons.",
				e->key);
			continue;
		}
		e->value = getenv(e->key);
	}
}
