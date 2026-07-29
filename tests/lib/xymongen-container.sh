#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Container-side helper for build/xymongen-podman.sh. It is not executable,
# so tests/testsuite does not discover it as a test.
#
# Unlike xymonnet-system-container.sh, this runs no network fixtures -- it
# builds a full Xymon server tree and runs the plain regression catalog
# (./tests/testsuite), matching the configure/build recipe .github/workflows/
# build.yml already uses for its "server" leg (see that file's "Run
# regression tests against the build" step), just inside a container instead
# of a GitHub runner.

set -euo pipefail

# shellcheck disable=SC1091
. /etc/os-release
case "$ID" in
	rocky|rhel|centos|almalinux|fedora) os_family=rhel ;;
	*)                                  os_family=debian ;;
esac

if [[ $os_family = rhel ]]; then
	echo 'keepcache=True' >> /etc/dnf/dnf.conf
	dnf install -y epel-release
	dnf config-manager --set-enabled crb
	dnf install -y gcc gcc-c++ make c-ares-devel openldap-devel pcre2-devel \
		rrdtool-devel openssl-devel libtirpc-devel openssl
	httpdgid=apache
else
	export DEBIAN_FRONTEND=noninteractive
	rm -f /etc/apt/apt.conf.d/docker-clean
	apt-get update
	apt-get install -y --no-install-recommends build-essential ca-certificates \
		libc-ares-dev libldap-dev libpcre2-dev librrd-dev libssl-dev \
		libtirpc-dev openssl
	httpdgid=www-data
fi

cp -a /src /work
cd /work

# Same values .github/workflows/build.yml passes to ./configure --server --
# every path-shaped var is pre-set precisely so configure.server's
# getent-passwd lookup for XYMONUSER is skipped (it only runs when
# XYMONTOPDIR is still unset) and every prompt is pre-answered, so this
# needs no real "xymon" system user and no interactive `yes ""` pipe.
export USEXYMONPING=y
export ENABLESSL=y
export ENABLELDAP=y
export ENABLELDAPSSL=y
export XYMONUSER=xymon
export XYMONTOPDIR=/usr/lib/xymon
export XYMONVAR=/var/lib/xymon
export XYMONHOSTURL=/xymon
export CGIDIR=/usr/lib/xymon/cgi-bin
export XYMONCGIURL=/xymon-cgi
export SECURECGIDIR=/usr/lib/xymon/cgi-secure
export SECUREXYMONCGIURL=/xymon-seccgi
export HTTPDGID=$httpdgid
export XYMONLOGDIR=/var/log/xymon
export XYMONHOSTNAME=localhost
export XYMONHOSTIP=127.0.0.1
export MANROOT=/usr/share/man
export INSTALLBINDIR=/usr/lib/xymon/server/bin
export INSTALLETCDIR=/etc/xymon
export INSTALLWEBDIR=/etc/xymon/web
export INSTALLEXTDIR=/usr/lib/xymon/server/ext
export INSTALLTMPDIR=/var/lib/xymon/tmp
export INSTALLWWWDIR=/var/lib/xymon/www

./configure --server

# Several Makefiles (lib/Makefile's standalone debug tools) and a couple of
# tests/web/*.sh scripts (which invoke $CC directly to compile their own
# throwaway harness) link straight from a prebuilt .a archive or object file
# without wiring $(LDFLAGS) into the link step -- only $(CC)/$(CFLAGS). On
# Ubuntu's default hardened gcc-13 that mismatch (objects effectively
# non-PIC, output linked as PIE) fails with "recompile with -fPIE". A wrapper
# binary, rather than baking the flags into $CC as a string, is required
# here: tests/web/*.sh do `command -v "$CC"`, which needs a single token, not
# "cc -fno-pie -no-pie".
cat > /usr/local/bin/xymon-nopie-cc <<EOF
#!/bin/sh
exec ${CC:-cc} -fno-pie -no-pie "\$@"
EOF
chmod +x /usr/local/bin/xymon-nopie-cc
export CC=xymon-nopie-cc
make -j"$(nproc)" CC="$CC"

./tests/testsuite
