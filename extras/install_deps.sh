#!/bin/sh
#
# Install the build dependencies of gluster-block on EL8/EL9
# (CentOS Stream, Rocky Linux, AlmaLinux), Fedora, Debian and Ubuntu.

set -e

[ "$(id -u)" -ne 0 ] && SUDO=sudo

. /etc/os-release
EL=${VERSION_ID%%.*}

case "$ID" in
debian|ubuntu)
	export DEBIAN_FRONTEND=noninteractive
	$SUDO apt-get update
	$SUDO apt-get install -y git gcc make autoconf automake libtool file \
		pkgconf rpcsvc-proto libtirpc-dev uuid-dev libjson-c-dev \
		libglusterfs-dev systemd debhelper dpkg-dev python3
	# systemd.pc moved to systemd-dev in Debian 13 and Ubuntu 24.04
	if apt-cache show systemd-dev >/dev/null 2>&1; then
		$SUDO apt-get install -y systemd-dev
	fi
	exit 0
	;;
fedora)
	;;
centos|rhel|rocky|almalinux|ol)
	$SUDO dnf install -y dnf-plugins-core epel-release
	case "$EL" in
	8)
		# libtirpc-devel, rpcgen and glusterfs-api-devel
		$SUDO dnf config-manager --set-enabled powertools
		;;
	9)
		$SUDO dnf config-manager --set-enabled crb
		# glusterfs-api-devel comes from the CentOS Storage SIG
		$SUDO dnf install -y centos-release-gluster11
		;;
	*)
		echo "EL$EL is not supported: there is no Gluster release for it" >&2
		exit 1
		;;
	esac
	;;
*)
	echo "$ID is not supported" >&2
	exit 1
	;;
esac

$SUDO dnf install -y git gcc make autoconf automake libtool file rpm-build \
	glusterfs-api-devel json-c-devel libuuid-devel libtirpc-devel rpcgen \
	systemd selinux-policy-devel bzip2 findutils python3 libmicrohttpd-devel
