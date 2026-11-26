# Image of the functional tests (tests/functional/run.sh): the build
# dependencies of gluster-block and gluster-block-apid, glusterd and the
# tools of the test cluster.
FROM rockylinux:9

COPY extras/install_deps.sh /tmp/install_deps.sh
RUN sh /tmp/install_deps.sh \
 && dnf install -y glusterfs-server glusterfs-fuse rpcbind attr procps-ng \
        iproute util-linux rsync python3 python3-jsonschema python3-pyyaml \
        libmicrohttpd-devel gnutls-devel jq \
 && dnf clean all
