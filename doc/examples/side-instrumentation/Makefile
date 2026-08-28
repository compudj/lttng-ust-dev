# SPDX-License-Identifier: MIT
#
# Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
#
# This makefile is not using automake so that users can see how to build
# an application instrumented with libside.
#
# The application depends on libside only. It does not link against
# LTTng-UST: the tracer is preloaded at run time, which
# run-side-example does.
#
# This makefile is purposefully kept simple to support GNU and BSD make.

SIDE_CFLAGS = `pkg-config --cflags libside 2>/dev/null`
SIDE_LIBS = `pkg-config --libs libside 2>/dev/null || echo -lside`
LOCAL_CPPFLAGS += -I.
AM_V_P := :

all: side-example

side-example.o: side-example.c
	@if $(AM_V_P); then set -x; else echo "  CC       $@"; fi; \
		$(CC) $(CPPFLAGS) $(LOCAL_CPPFLAGS) $(AM_CPPFLAGS) $(AM_CFLAGS) \
		$(SIDE_CFLAGS) $(CFLAGS) -c -o $@ $<

side-example: side-example.o
	@if $(AM_V_P); then set -x; else echo "  CCLD     $@"; fi; \
		$(CC) -o $@ $(LDFLAGS) $(AM_CFLAGS) $(AM_LDFLAGS) $(CFLAGS) \
		side-example.o $(SIDE_LIBS)

.PHONY: clean
clean:
	rm -f *.o side-example
