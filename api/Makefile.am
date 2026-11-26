sbin_PROGRAMS = gluster-block-apid

gluster_block_apid_SOURCES = apid.c backend.c v1.c

noinst_HEADERS = apid.h

gluster_block_apid_CFLAGS = $(MHD_CFLAGS) $(JSONC_CFLAGS) $(TIRPC_CFLAGS)   \
                            -DGB_DAEMON_SOCKET=\"$(localstatedir)/run/gluster-blockd.socket\" \
                            -DGB_LOGDIR_DEF=\"$(localstatedir)/log/gluster-block\" \
                            -DGB_API_SYSCONFIG=\"$(sysconfigdir)/gluster-block-apid\" \
                            -I$(top_builddir)/ -I$(top_srcdir)/client         \
                            -I$(top_builddir)/rpc/rpcl -I$(top_srcdir)/rpc/rpcl

gluster_block_apid_LDADD = $(top_builddir)/client/libgbclient.la            \
                           $(top_builddir)/rpc/rpcl/libgbrpcxdr.la          \
                           $(MHD_LIBS) $(JSONC_LIBS) $(TIRPC_LIBS) $(PTHREAD)

DISTCLEANFILES = Makefile.in

CLEANFILES = *~
