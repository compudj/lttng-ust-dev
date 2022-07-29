/*
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (C) 2022 Michael Jeanson <mjeanson@efficios.com>
 */

#include <stdio.h>

#include "tap.h"

#include "common/smp.h"

int main(void)
{
	int ret;

	plan_tests(2);

	ret = get_possible_cpus_array_len();
	ok(ret > 0, "get_possible_cpus_array_len (%d > 0)", ret);

	ret = get_num_possible_cpus_fallback();
	ok(ret > 0, "get_num_possible_cpus_fallback (%d > 0)", ret);

	return exit_status();
}
