noinst_PROGRAMS = gfapi-test
gfapi_test_SOURCES = gfapi-test.c

gfapi_test_CFLAGS = $(GFAPI_CFLAGS)
gfapi_test_LDADD = $(GFAPI_LIBS)

TESTS = compat/rpc-check.sh

# a release adds its block.x here: tests/compat/rpc-check.py --snapshot X.Y.Z
RPC_BASELINES = compat/rpc/block-v0.5.27.x compat/rpc/block-v0.5.33.x

EXTRA_DIST = compat/rpc-check.sh compat/rpc-check.py $(RPC_BASELINES)

DISTCLEANFILES = Makefile.in

CLEANFILES = *~
