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
# Test cluster in one privileged container, without LIO (run by run.sh):
#
# - builds and installs the tree in /src (copied to /build),
# - glusterd with volumes "rv" (replica 3), "dv" (disperse 3, redundancy 1)
#   and "plain" (no blocks), all bricks in the container,
# - host 1 is the container itself, hosts 2..N are network namespaces with
#   their own /run, configfs (tmpfs) and gluster-blockd,
# - targetcli is tests/functional/targetcli-stub.py, tcmu-runner a sleep,
# - gluster-block-apid on its unix socket when it was built.
#
# Sourced by the tests, exports HOSTS (comma separated), HOST1..HOSTN.
#
#   GB_TEST_HOSTS   number of hosts (default 3)
#   GB_TEST_SRC     source tree (default /src)

set -u

GB_TEST_HOSTS=${GB_TEST_HOSTS:-3}
GB_TEST_SRC=${GB_TEST_SRC:-/src}
OUT=${GB_TEST_OUT:-/tmp/gbtest}
FT=$GB_TEST_SRC/tests/functional
mkdir -p "$OUT"

log() { echo "### $*"; }
die() { echo "SETUP FAILED: $*" >&2; exit 1; }

build() {
	log "build"
	git config --global --add safe.directory '*'
	rm -rf /build && mkdir -p /build
	(cd "$GB_TEST_SRC" && git ls-files -co --exclude-standard) > /tmp/files
	rsync -a --files-from=/tmp/files "$GB_TEST_SRC"/ /build/
	cp -a "$GB_TEST_SRC"/.git /build/.git
	cd /build || die "no /build"
	./autogen.sh > "$OUT/autogen.log" 2>&1 || die "autogen, see $OUT/autogen.log"
	./configure --prefix=/usr --localstatedir=/var --sysconfdir=/etc \
		CFLAGS="-O2 -g -Wall -Werror -Wno-unused-but-set-variable" \
		> "$OUT/configure.log" 2>&1 || die "configure, see $OUT/configure.log"
	make -j"$(nproc)" > "$OUT/make.log" 2>&1 || { grep -E "error|warning" "$OUT/make.log"; die "make"; }
	make install > "$OUT/install.log" 2>&1 || die "make install"
	cd / || exit 1
}

stubs() {
	install -m 0755 "$FT/targetcli-stub.py" /usr/bin/targetcli
	# gluster-blockd asks 'tcmu-runner --version' and looks for a process
	# named tcmu-runner (pgrep -x): a script answers, a copy of sleep runs
	printf '#!/bin/sh\necho "tcmu-runner 1.6.2"\n' > /usr/bin/tcmu-runner
	chmod 0755 /usr/bin/tcmu-runner
	mkdir -p /opt/gbtest/bin
	cp /usr/bin/sleep /opt/gbtest/bin/tcmu-runner
	pgrep -x tcmu-runner >/dev/null || /opt/gbtest/bin/tcmu-runner infinity &
}

gluster_up() {
	log "glusterd and volumes"
	IP=$(hostname -i | awk '{print $1}')
	rpcbind -w 2>/dev/null || rpcbind
	glusterd || die "glusterd"
	for i in $(seq 1 30); do gluster --mode=script peer status >/dev/null 2>&1 && break; sleep 1; done
	rm -rf /bricks && mkdir -p /bricks
	gluster --mode=script volume create rv replica 3 \
		$IP:/bricks/r1 $IP:/bricks/r2 $IP:/bricks/r3 force >/dev/null || die "create rv"
	gluster --mode=script volume create dv disperse 3 redundancy 1 \
		$IP:/bricks/d1 $IP:/bricks/d2 $IP:/bricks/d3 force >/dev/null || die "create dv"
	gluster --mode=script volume create plain $IP:/bricks/p1 force >/dev/null || die "create plain"
	for v in rv dv plain; do gluster --mode=script volume start $v >/dev/null || die "start $v"; done
	sleep 3
}

config() {
	mkdir -p /etc/sysconfig /var/log/gluster-block /var/run/gluster-block
	cat > /etc/sysconfig/gluster-blockd <<-EOF
	GB_BHV_VOLSERVER="$IP"
	GB_LOG_LEVEL=DEBUG
	GB_MAINT_IO_IDLE_SEC=1
	GB_MAINT_IO_WAIT_SEC=10
	EOF
}

# host 1: the container, configfs is a tmpfs
host1() {
	mkdir -p /tmp/cfg1 && mount -t tmpfs tmpfs /tmp/cfg1
	mount --bind /tmp/cfg1 /sys/kernel/config
	mkdir -p /sys/kernel/config/target/core /sys/kernel/config/target/iscsi
	gluster-blockd --log-level DEBUG > "$OUT/blockd-1.out" 2>&1 &
	wait_port "" 24010 || die "gluster-blockd of host 1"
	for i in $(seq 1 20); do [ -S /var/run/gluster-blockd.socket ] && break; sleep 1; done
}

wait_port() { # netns port
	local i
	for i in $(seq 1 30); do
		if [ -n "$1" ]; then
			ip netns exec "$1" ss -ltn 2>/dev/null | grep -q ":$2 " && return 0
		else
			ss -ltn | grep -q ":$2 " && return 0
		fi
		sleep 1
	done
	return 1
}

# host N: a network namespace with its own /run, configfs and logs
hostn() {
	local n=$1 ns=gbh$1 net=10.$((90 + $1)).0
	ip netns add $ns
	ip link add ${ns}a type veth peer name ${ns}b
	ip link set ${ns}b netns $ns
	ip addr add $net.1/24 dev ${ns}a && ip link set ${ns}a up
	ip netns exec $ns ip addr add $net.2/24 dev ${ns}b
	ip netns exec $ns ip link set ${ns}b up
	ip netns exec $ns ip link set lo up
	ip netns exec $ns ip route add default via $net.1
	mkdir -p /tmp/log$n /tmp/run$n
	echo $net.2
}

hostn_start() {
	local n=$1 ns=gbh$1
	# 'ip netns exec' mounts a new /sys, mount the configfs of the host on it;
	# started from a subshell, it is not a job of the test shell
	(ip netns exec $ns unshare -m sh -c "
		mount -t tmpfs tmpfs /run && mkdir -p /run/gluster-block
		mount --bind /tmp/log$n /var/log/gluster-block
		mount -t tmpfs tmpfs /sys/kernel/config
		mkdir -p /sys/kernel/config/target/core /sys/kernel/config/target/iscsi
		mount --bind /run /var/run 2>/dev/null
		rpcbind -w 2>/dev/null || rpcbind
		echo \$\$ > /tmp/run$n/pid
		exec gluster-blockd --log-level DEBUG" > "$OUT/blockd-$n.out" 2>&1 &)
	wait_port $ns 24010 || die "gluster-blockd of host $n"
}

# stop or start gluster-blockd of host N (N > 1), for down host tests
host_down() { ip netns pids gbh$1 | xargs -r kill -9; sleep 1; }
host_up() { hostn_start "$1"; }

# run a command in the mount namespace of host N (its configfs)
on_host() { # n cmd...
	local n=$1; shift
	if [ "$n" = 1 ]; then "$@"; return; fi
	nsenter -t "$(cat /tmp/run$n/pid)" -m -n "$@"
}

apid_up() {
	if [ -x /usr/sbin/gluster-block-apid ]; then
		log "gluster-block-apid"
		gluster-block-apid --log-level DEBUG > "$OUT/apid.out" 2>&1 &
		for i in $(seq 1 20); do [ -S /run/gluster-block/api.sock ] && return 0; sleep 1; done
		die "gluster-block-apid does not listen"
	fi
}

setup_all() {
	build
	stubs
	gluster_up
	config
	host1
	HOSTS=$IP
	HOST1=$IP
	export HOST1
	for n in $(seq 2 "$GB_TEST_HOSTS"); do
		h=$(hostn "$n")
		eval "HOST$n=$h"
		export "HOST$n"
		HOSTS="$HOSTS,$h"
		hostn_start "$n"
	done
	export HOSTS IP
	apid_up
	log "hosts: $HOSTS"
}
