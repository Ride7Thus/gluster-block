noinst_PROGRAMS = gfapi-test
gfapi_test_SOURCES = gfapi-test.c

gfapi_test_CFLAGS = $(GFAPI_CFLAGS)
gfapi_test_LDADD = $(GFAPI_LIBS)

TESTS = compat/rpc-check.sh

# a release adds its block.x here: tests/compat/rpc-check.py --snapshot X.Y.Z
RPC_BASELINES = compat/rpc/block-v0.5.27.x compat/rpc/block-v0.5.33.x

FUNCTIONAL = functional/Dockerfile functional/run.sh functional/inside.sh \
             functional/setup.sh functional/targetcli-stub.py \
             functional/gbtest.py functional/test_api.py \
             functional/test_api_write.py functional/test_api_tls.py

EXTRA_DIST = compat/rpc-check.sh compat/rpc-check.py $(RPC_BASELINES) \
             compat/json/daemon.schema.json compat/api-check.sh $(FUNCTIONAL)

DISTCLEANFILES = Makefile.in

CLEANFILES = *~
