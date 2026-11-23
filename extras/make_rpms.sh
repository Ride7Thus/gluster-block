#!/bin/sh
#
# Build gluster-block RPMs from a git checkout.
#
#   extras/make_rpms.sh [rpmbuild options]
#
# The packages land in build/rpmbuild/{RPMS,SRPMS}. Build dependencies on
# EL8/EL9 (CentOS Stream, Rocky Linux, AlmaLinux) are installed with
# extras/install_deps.sh.

set -e

TOPDIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOPDIR"

if [ ! -e .git ]; then
	echo "make_rpms.sh: $TOPDIR is not a git checkout" >&2
	exit 1
fi

./autogen.sh
./configure
make rpms RPMBUILD_ARGS="$*"

echo "RPMs:"
find build/rpmbuild/RPMS build/rpmbuild/SRPMS -name '*.rpm'
