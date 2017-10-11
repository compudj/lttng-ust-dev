AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_builddir)/include
AM_CFLAGS += -fno-strict-aliasing

noinst_LTLIBRARIES = liblttng-rseq.la

liblttng_rseq_la_SOURCES = \
	rseq.c

libringbuffer_la_LIBADD = \
	-lpthread \
	-lrt

libringbuffer_la_CFLAGS = -DUST_COMPONENT="liblttng_rseq" $(AM_CFLAGS)
