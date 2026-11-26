noinst_LTLIBRARIES = libgbclient.la

libgbclient_la_SOURCES = gb-client.c

noinst_HEADERS = gb-client.h

libgbclient_la_CFLAGS = $(TIRPC_CFLAGS) -I$(top_builddir)/ \
                        -I$(top_builddir)/rpc/rpcl -I$(top_srcdir)/rpc/rpcl

DISTCLEANFILES = Makefile.in

CLEANFILES = *~
