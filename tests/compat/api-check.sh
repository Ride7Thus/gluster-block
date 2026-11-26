#!/bin/sh
#
# Copyright (c) 2026 gluster-block contributors
# This file is part of gluster-block.
#
# This file is licensed to you under your choice of the GNU Lesser
# General Public License, version 3 or any later version (LGPLv3 or
# later), or the GNU General Public License, version 2 (GPLv2), in all
# cases as published by the Free Software Foundation.
#
# API v1 changes only compatibly: compare docs/api/openapi.yaml with the one
# of a git ref with oasdiff (breaking changes fail).
#
#   tests/compat/api-check.sh [<git ref>]
#
# Default ref: the last vX.Y.Z tag that has docs/api/openapi.yaml. Exit 0
# when it is compatible or there is nothing to compare with, 1 when it
# breaks, 77 when oasdiff (or docker/podman to run it) is missing.
#
#   OASDIFF_IMAGE   container image of oasdiff [tufin/oasdiff:latest]

set -e

TOP=$(cd "$(dirname "$0")/../.." && pwd)
SPEC=docs/api/openapi.yaml
IMAGE=${OASDIFF_IMAGE:-tufin/oasdiff:latest}

cd "$TOP"

REF=${1:-}
if [ -z "$REF" ]; then
	for t in $(git tag --list 'v[0-9]*' --sort=-v:refname); do
		if git cat-file -e "$t:$SPEC" 2>/dev/null; then
			REF=$t
			break
		fi
	done
	if [ -z "$REF" ]; then
		echo "api-check: no release has $SPEC yet, nothing to compare"
		exit 0
	fi
fi
if ! git cat-file -e "$REF:$SPEC" 2>/dev/null; then
	echo "api-check: $REF has no $SPEC, nothing to compare"
	exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
git show "$REF:$SPEC" > "$WORK/base.yaml"
cp "$SPEC" "$WORK/new.yaml"

if command -v oasdiff >/dev/null 2>&1; then
	RUN="oasdiff"
	BASE=$WORK/base.yaml
	NEW=$WORK/new.yaml
else
	ENGINE=$(command -v podman || command -v docker || true)
	if [ -z "$ENGINE" ]; then
		echo "api-check: neither oasdiff nor podman/docker found, skipped"
		exit 77
	fi
	RUN="$ENGINE run --rm -v $WORK:/work:Z $IMAGE"
	BASE=/work/base.yaml
	NEW=/work/new.yaml
fi

echo "api-check: $SPEC against $REF"
if $RUN breaking "$BASE" "$NEW" --fail-on ERR; then
	echo "api-check: compatible with $REF"
else
	echo "api-check: $SPEC breaks API v1 of $REF. Clients of v1 must keep"
	echo "working: add fields and endpoints instead of changing them."
	exit 1
fi
