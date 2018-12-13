AM_CPPFLAGS = -I$(top_srcdir)/include
AM_CFLAGS += -fno-strict-aliasing

noinst_LTLIBRARIES = liblttng-ust-rcu.la
liblttng_ust_rcu_la_SOURCES = \
	lttng-ust-rcu.h \
	lttng-ust-rcu.c

liblttng_ust_rcu_la_LDFLAGS = -no-undefined -static
liblttng_ust_rcu_la_CFLAGS = -DUST_COMPONENT="lttng_ust_rcu" -fPIC $(AM_CFLAGS)
