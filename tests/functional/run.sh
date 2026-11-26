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
# Run the functional tests in a privileged container (docker or podman):
# a gluster volume, several gluster-blockd "hosts" in network namespaces,
# no LIO (see setup.sh).
#
#   tests/functional/run.sh [test ...]    default: all tests/functional/test_*.py
#
#   GB_TEST_OUT=dir   keep logs and the recorded JSON fixtures in dir
#                     (default build/functional)
#   GB_TEST_SHELL=1   open a shell in the cluster after the tests

set -e

TOP=$(cd "$(dirname "$0")/../.." && pwd)
ENGINE=${ENGINE:-$(command -v podman || command -v docker)}
IMAGE=${GB_TEST_IMAGE:-gluster-block-functional}
OUT=${GB_TEST_OUT:-$TOP/build/functional}

[ -n "$ENGINE" ] || { echo "run.sh: neither podman nor docker found" >&2; exit 1; }

"$ENGINE" build -q -t "$IMAGE" -f "$TOP/tests/functional/Dockerfile" "$TOP" >/dev/null

mkdir -p "$OUT"
TTY=
[ -n "${GB_TEST_SHELL:-}" ] && TTY=-it

exec "$ENGINE" run --rm $TTY --privileged --hostname gbtest \
	-e GB_TEST_HOSTS="${GB_TEST_HOSTS:-3}" -e GB_TEST_SHELL="${GB_TEST_SHELL:-}" \
	-e GB_TEST_OUT=/out \
	-v "$TOP":/src:ro -v "$OUT":/out \
	"$IMAGE" /src/tests/functional/inside.sh "$@"
