/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2011-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (C) 2011-2012 Matthew Khouzam <matthew.khouzam@ericsson.com>
 */

/*
 * Sample lttng-ust tracepoint provider.
 */

/*
 * First part: defines
 * We undef a macro before defining it as it can be used in several files.
 */

/*
 * Must be included before include tracepoint provider
 * ex.: project_event
 * ex.: project_component_event
 *
 * Optional company name goes here
 * ex.: com_efficios_project_component_event
 *
 * In this example, "sample" is the project, and "component" is the
 * component.
 */
#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER sample_component

/*
 * include file (this files's name)
 */
#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./sample_component_provider.h"

/*
 * Add this precompiler conditionals to ensure the tracepoint event generation
 * can include this file more than once.
 */
#if !defined(_SAMPLE_COMPONENT_PROVIDER_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _SAMPLE_COMPONENT_PROVIDER_H
/*
 * Add this to allow programs to call "tracepoint(...):
 */
#include <lttng/tracepoint.h>

/*
 * The following tracepoint event writes a message (c string) into the
 * field message of the trace event message in the provider
 * sample_component in other words:
 *
 *    sample_component:message:message = text.
 */
LTTNG_UST_TRACEPOINT_EVENT(
	/*
	 * provider name, not a variable but a string starting with a letter
	 * and containing either letters, numbers or underscores.
	 * Needs to be the same as LTTNG_UST_TRACEPOINT_PROVIDER
	 */
	sample_component,
	/*
	 * tracepoint name, same format as sample provider. Does not need to be
	 * declared before. in this case the name is "message"
	 */
	message,
	/*
	 * LTTNG_UST_TP_ARGS macro contains the arguments passed for the tracepoint
	 * it is in the following format
	 * 		LTTNG_UST_TP_ARGS( type1, name1, type2, name2, ... type10, name10)
	 * where there can be from zero to ten elements.
	 * typeN is the datatype, such as int, struct or double **.
	 * name is the variable name (in "int myInt" the name would be myint)
	 * 		LTTNG_UST_TP_ARGS() is valid to mean no arguments
	 * 		LTTNG_UST_TP_ARGS( void ) is valid too
	 */
	LTTNG_UST_TP_ARGS(const char *, text),
	/*
	 * LTTNG_UST_TP_FIELDS describes how to write the fields of the trace event.
	 * You can use the args here
	 */
	LTTNG_UST_TP_FIELDS(
	/*
	 * The lttng_ust_field_string macro takes a c string and writes it into a field
	 * named "message"
	 */
		lttng_ust_field_string(message, text)
	)
)
/*
 * Trace loglevel, shows the level of the trace event. It can be
 * LTTNG_UST_TRACEPOINT_LOGLEVEL_EMERG, LTTNG_UST_TRACEPOINT_LOGLEVEL_ALERT,
 * LTTNG_UST_TRACEPOINT_LOGLEVEL_CRIT, LTTNG_UST_TRACEPOINT_LOGLEVEL_ERR,
 * LTTNG_UST_TRACEPOINT_LOGLEVEL_WARNING, LTTNG_UST_TRACEPOINT_LOGLEVEL_INFO or
 * others.  If this is not set, LTTNG_UST_TRACEPOINT_LOGLEVEL_DEFAULT is
 * assumed.  The first two arguments identify the tracepoint See details in
 * <lttng/tracepoint.h> line 347
 */
LTTNG_UST_TRACEPOINT_LOGLEVEL(
       /*
        * The provider name, must be the same as the provider name in the
        * LTTNG_UST_TRACEPOINT_EVENT and as LTTNG_UST_TRACEPOINT_PROVIDER above.
        */
	sample_component,
       /*
        * The tracepoint name, must be the same as the tracepoint name in the
        * LTTNG_UST_TRACEPOINT_EVENT
        */
	message,
       /*
        * The tracepoint loglevel. Warning, some levels are abbreviated and
        * others are not, please see <lttng/tracepoint.h>
        */
	LTTNG_UST_TRACEPOINT_LOGLEVEL_WARNING)

#endif /* _SAMPLE_COMPONENT_PROVIDER_H */

/*
 * Add this after defining the tracepoint events to expand the macros.
 */
#include <lttng/tracepoint-event.h>
