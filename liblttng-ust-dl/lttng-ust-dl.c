/*
 * Copyright (C) 2013  Paul Woegerer <paul.woegerer@mentor.com>
 * Copyright (C) 2015  Antoine Busque <abusque@efficios.com>
 * Copyright (C) 2016  Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1 of
 * the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE
#include <limits.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <lttng/ust-dlfcn.h>
#include <lttng/ust-elf.h>
#include <lttng/ust-events.h>
#include <helper.h>
#include "usterr-signal-safe.h"

/* Include link.h last else it conflicts with ust-dlfcn. */
#include <link.h>

static void *(*__lttng_ust_plibc_dlopen)(const char *filename, int flag);
static int (*__lttng_ust_plibc_dlclose)(void *handle);

static
void *_lttng_ust_dl_libc_dlopen(const char *filename, int flag)
{
	if (!__lttng_ust_plibc_dlopen) {
		__lttng_ust_plibc_dlopen = dlsym(RTLD_NEXT, "dlopen");
		if (!__lttng_ust_plibc_dlopen) {
			fprintf(stderr, "%s\n", dlerror());
			return NULL;
		}
	}
	return __lttng_ust_plibc_dlopen(filename, flag);
}

static
int _lttng_ust_dl_libc_dlclose(void *handle)
{
	if (!__lttng_ust_plibc_dlclose) {
		__lttng_ust_plibc_dlclose = dlsym(RTLD_NEXT, "dlclose");
		if (!__lttng_ust_plibc_dlclose) {
			fprintf(stderr, "%s\n", dlerror());
			return -1;
		}
	}
	return __lttng_ust_plibc_dlclose(handle);
}

void *dlopen(const char *filename, int flag)
{
	void *handle;

	handle = _lttng_ust_dl_libc_dlopen(filename, flag);
	lttng_ust_dl_update(LTTNG_UST_CALLER_IP());
	return handle;
}

int dlclose(void *handle)
{
	int ret;

	ret = _lttng_ust_dl_libc_dlclose(handle);
	lttng_ust_dl_update(LTTNG_UST_CALLER_IP());
	return ret;
}
