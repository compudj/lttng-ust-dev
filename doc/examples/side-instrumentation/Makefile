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
#
# Note that no -std is given, in C or in C++. The instrumentation macros
# rely on the `, ## __VA_ARGS__` extension to leave out an optional
# attribute list, so they need the GNU dialect a compiler uses by
# default: -std=c11 and -std=c++17 both fail to compile them, where
# -std=gnu11 and -std=gnu++17 succeed. -Wpedantic does not work on the
# C++ program either: an empty attribute list is a zero size array,
# which C++ rejects and C accepts.

SIDE_CFLAGS = `pkg-config --cflags libside 2>/dev/null`
SIDE_LIBS = `pkg-config --libs libside 2>/dev/null || echo -lside`
LOCAL_CPPFLAGS += -I.
AM_V_P := :

all: side-example side-example-cxx

side-example.o: side-example.c
	@if $(AM_V_P); then set -x; else echo "  CC       $@"; fi; \
		$(CC) $(CPPFLAGS) $(LOCAL_CPPFLAGS) $(AM_CPPFLAGS) $(AM_CFLAGS) \
		$(SIDE_CFLAGS) $(CFLAGS) -c -o $@ $<

side-example: side-example.o
	@if $(AM_V_P); then set -x; else echo "  CCLD     $@"; fi; \
		$(CC) -o $@ $(LDFLAGS) $(AM_CFLAGS) $(AM_LDFLAGS) $(CFLAGS) \
		side-example.o $(SIDE_LIBS)

side-example-cxx.o: side-example-cxx.cpp
	@if $(AM_V_P); then set -x; else echo "  CXX      $@"; fi; \
		$(CXX) $(CPPFLAGS) $(LOCAL_CPPFLAGS) $(AM_CPPFLAGS) $(AM_CXXFLAGS) \
		$(SIDE_CFLAGS) $(CXXFLAGS) -c -o $@ $<

side-example-cxx: side-example-cxx.o
	@if $(AM_V_P); then set -x; else echo "  CXXLD    $@"; fi; \
		$(CXX) -o $@ $(LDFLAGS) $(AM_CXXFLAGS) $(AM_LDFLAGS) $(CXXFLAGS) \
		side-example-cxx.o $(SIDE_LIBS)

.PHONY: clean
clean:
	rm -f *.o side-example side-example-cxx
