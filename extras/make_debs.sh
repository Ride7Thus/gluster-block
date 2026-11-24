#!/bin/sh
#
# Build gluster-block .deb packages from a git checkout.
#
#   extras/make_debs.sh [dpkg-buildpackage options]
#
# The packages land in build/debs. Build dependencies on Debian and Ubuntu
# are installed with extras/install_deps.sh.
#
# The version comes from version-gen.sh plus the distribution, like the dist
# tag of the RPMs: 0.5.25+debian12, 0.5.25+ubuntu24.04.

set -e

TOPDIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOPDIR"

if [ ! -e .git ]; then
	echo "make_debs.sh: $TOPDIR is not a git checkout" >&2
	exit 1
fi

. /etc/os-release
VERSION="$(./version-gen.sh)+${ID}${VERSION_ID}"

# debian/changelog is generated, the history is in NEWS and git
cat > debian/changelog <<END
gluster-block (${VERSION}) ${VERSION_CODENAME:-unstable}; urgency=medium

  * Build of $(git describe --always --tags), see NEWS.

 -- $(sed -n 's/^Maintainer: //p' debian/control)  $(date -R)
END

dpkg-buildpackage -us -uc -b "$@"

mkdir -p build/debs
mv ../gluster-block*_"${VERSION}"_*.deb ../gluster-block*_"${VERSION}"_*.ddeb \
   ../gluster-block_"${VERSION}"_*.buildinfo ../gluster-block_"${VERSION}"_*.changes \
   build/debs/ 2>/dev/null || true

echo "packages:"
ls -1 build/debs/*.deb
