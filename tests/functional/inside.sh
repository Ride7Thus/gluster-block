#!/bin/bash
#
# Copyright (c) 2026 gluster-block contributors
# This file is part of gluster-block.
#
# This file is licensed to you under your choice of the GNU Lesser
# General Public License, version 3 or any later version (LGPLv3 or
# later), or the GNU General Public License, version 2 (GPLv2), in all
# cases as published by the Free Software Foundation.
#
# Entry point in the test container (see run.sh): set the cluster up, run
# the tests, keep the logs in $GB_TEST_OUT.

. /src/tests/functional/setup.sh

setup_all

rc=0
tests=("$@")
[ ${#tests[@]} -eq 0 ] && tests=(/src/tests/functional/test_*.py)
for t in "${tests[@]}"; do
	[ -e "$t" ] || t=/src/tests/functional/$t
	log "$(basename "$t")"
	python3 -I "$t" -v || rc=1
done

cp /var/log/gluster-block/*.log "$GB_TEST_OUT/" 2>/dev/null
for n in $(seq 2 "$GB_TEST_HOSTS"); do
	for f in /tmp/log$n/*.log; do
		[ -e "$f" ] && cp "$f" "$GB_TEST_OUT/host$n-$(basename "$f")"
	done
done

if [ -n "${GB_TEST_SHELL:-}" ]; then
	log "shell in the test cluster, exit to stop it"
	bash -i
fi

[ $rc -eq 0 ] && log "functional tests passed" || log "functional tests FAILED"
exit $rc
