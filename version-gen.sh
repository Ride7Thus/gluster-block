#!/bin/sh
#
# Print the package version.
#
#  - a VERSION file (release tarballs) wins;
#  - else the last "vX.Y.Z" tag, like "0.5.1" or "0.5.1.12.gabcdef0" when
#    there are commits after it;
#  - else (a fork without tags) the base version below in the same format.

BASE_VERSION=0.5.21

cd "$(dirname "$0")" || exit 1

if [ -f VERSION ]; then
	cat VERSION
	exit 0
fi

if V=$(git describe --tags --match 'v[0-9]*' 2>/dev/null); then
	# v0.5.1-12-gabcdef0 -> 0.5.1.12.gabcdef0
	echo "$V" | sed -e 's/^v//' -e 's/-/./g'
elif C=$(git rev-list --count HEAD 2>/dev/null); then
	echo "$BASE_VERSION.$C.g$(git rev-parse --short HEAD)"
else
	echo "$BASE_VERSION"
fi
