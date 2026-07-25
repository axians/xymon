#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Container-side helper for build/xymonnet-valgrind-podman.sh. It is not
# executable, so tests/testsuite does not discover it as a regression test.

set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
	build-essential ca-certificates libc-ares-dev libldap2-dev libpcre2-dev \
	librrd-dev libssl-dev libtirpc-dev ldap-utils openssl python3 slapd valgrind

cp -a /src /work
cd /work

export XYMONUSER=xymon
export XYMONTOPDIR=/opt/xymon
export XYMONHOSTURL=/xymon
export CGIDIR=/opt/xymon/cgi-bin
export XYMONCGIURL=/xymon-cgi
export SECURECGIDIR=/opt/xymon/cgi-secure
export SECUREXYMONCGIURL=/xymon-seccgi
export HTTPDGID=www-data
export XYMONLOGDIR=/work/runtime/logs
export XYMONHOSTNAME=localhost
export XYMONHOSTIP=127.0.0.1
export MANROOT=/usr/share/man

set +o pipefail
yes "" | ./configure.server --caresinclude /nonexistent
set -o pipefail
make -j"$(nproc)" xymonnet-build

. /work/tests/lib/xymonnet-ldap-fixture.sh

XYMONNET=/work/xymonnet/xymonnet XYMONNET_VALGRIND="${XYMONNET_VALGRIND:-1}" \
	/work/tests/network/xymonnet-loopback.sh