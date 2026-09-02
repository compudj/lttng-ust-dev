# SPDX-FileCopyrightText: 2026 EfficiOS, Inc
#
# SPDX-License-Identifier: LGPL-2.1-only

AM_CPPFLAGS += -I$(srcdir)

noinst_PROGRAMS = bench-side bench-tp

# The libside program does not link against the tracer: run-benchmark
# preloads it, which is how libside instrumentation is used.
#
# TODO: make libside dependency optional
bench_side_SOURCES = bench-side.c
bench_side_LDADD = $(SIDE_LIBS)

# Both programs are built with _LGPL_SOURCE, which is what an
# application which may use the inlined RCU read side does, and which is
# what makes the tracepoint cost what it costs: without it the read side
# is a call into the wrapper rather than inlined code. Inlining it is
# what makes liblttng-ust-common a direct dependency here.
bench_tp_SOURCES = bench-tp.c tp.c ust_tests_benchmark_side.h
bench_tp_LDADD = \
	$(top_builddir)/src/lib/lttng-ust/liblttng-ust.la \
	$(top_builddir)/src/lib/lttng-ust-common/liblttng-ust-common.la \
	$(DL_LIBS)

dist_noinst_SCRIPTS = run-benchmark

EXTRA_DIST = README.md
