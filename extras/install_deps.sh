#!/bin/sh
#
# Install the build dependencies of gluster-block on EL8/EL9
# (CentOS Stream, Rocky Linux, AlmaLinux) and Fedora.

set -e

[ "$(id -u)" -ne 0 ] && SUDO=sudo

. /etc/os-release
EL=${VERSION_ID%%.*}

case "$ID" in
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
	systemd
