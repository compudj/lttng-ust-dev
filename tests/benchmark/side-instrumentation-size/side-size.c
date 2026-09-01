/*
 * SPDX-FileCopyrightText: 2026 EfficiOS, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * An application whose whole content is libside instrumentation: the
 * generated event definitions, and with CALLSITES defined, one call
 * site per event. There is nothing else to attribute a section to.
 */

#define _LGPL_SOURCE

#include "side-events.h"

int main(void)
{
#ifdef CALLSITES
#include "side-callsites.h"
#endif
	return 0;
}
