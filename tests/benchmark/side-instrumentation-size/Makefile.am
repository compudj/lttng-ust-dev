# SPDX-FileCopyrightText: 2026 EfficiOS, Inc
#
# SPDX-License-Identifier: LGPL-2.1-only

AM_CPPFLAGS += -I$(srcdir) -I$(builddir)

# How many events the programs are built with. Each has between one and
# eight fields, and no two have the same shape.
NR_EVENTS = 1000

BUILT_SOURCES = tp-events.h tp-callsites.h side-events.h side-callsites.h
CLEANFILES = $(BUILT_SOURCES) gen-instrumentation.stamp

gen-instrumentation.stamp: $(srcdir)/gen-instrumentation
	$(AM_V_GEN)$(srcdir)/gen-instrumentation --events $(NR_EVENTS) \
		--output-dir $(builddir) >/dev/null && touch $@

$(BUILT_SOURCES): gen-instrumentation.stamp

UST_LIBS = \
	$(top_builddir)/src/lib/lttng-ust/liblttng-ust.la \
	$(top_builddir)/src/lib/lttng-ust-common/liblttng-ust-common.la \
	$(DL_LIBS)

# Six programs: the tracepoints with and without their probe providers
# compiled in, each with and without call sites, and libside with and
# without call sites. libside has no probe provider to attribute: the
# description of an event is all there is.
#
# TODO: make libside dependency optional
noinst_PROGRAMS = \
	tp-defs tp-defs-probes tp-calls tp-calls-probes \
	side-defs side-calls

# LTTNG_UST_TRACEPOINT_PROBE_DYNAMIC_LINKAGE leaves the provider out of
# the program, which is what tells the cost of the probes apart from the
# cost of the definitions and of the call sites.
tp_defs_SOURCES = tp-size.c
tp_defs_CPPFLAGS = -DLTTNG_UST_TRACEPOINT_PROBE_DYNAMIC_LINKAGE $(AM_CPPFLAGS)
tp_defs_LDADD = $(UST_LIBS)

tp_calls_SOURCES = tp-size.c
tp_calls_CPPFLAGS = -DCALLSITES -DLTTNG_UST_TRACEPOINT_PROBE_DYNAMIC_LINKAGE $(AM_CPPFLAGS)
tp_calls_LDADD = $(UST_LIBS)

tp_defs_probes_SOURCES = tp-size.c tp-probes.c
tp_defs_probes_LDADD = $(UST_LIBS)

tp_calls_probes_SOURCES = tp-size.c tp-probes.c
tp_calls_probes_CPPFLAGS = -DCALLSITES $(AM_CPPFLAGS)
tp_calls_probes_LDADD = $(UST_LIBS)

side_defs_SOURCES = side-size.c
side_defs_LDADD = $(SIDE_LIBS)

side_calls_SOURCES = side-size.c
side_calls_CPPFLAGS = -DCALLSITES $(AM_CPPFLAGS)
side_calls_LDADD = $(SIDE_LIBS)

dist_noinst_SCRIPTS = gen-instrumentation measure-size

EXTRA_DIST = README.md
