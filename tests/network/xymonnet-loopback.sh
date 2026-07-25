#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Exercise the full xymonnet binary against deterministic loopback network
# fixtures. Set XYMONNET_VALGRIND=1 to run the same scenario under Memcheck.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/../lib/assert.sh"

require_bin XYMONNET xymonnet/xymonnet
command -v python3 >/dev/null 2>&1 || skip "python3 not found"

root=$(find_root)
fixture="$root/tests/fixtures/xymonnet-loopback.py"
protocols="$root/xymonnet/protocols.cfg"
assert_file_exists "$fixture"
assert_file_exists "$protocols"

work=$(mktempdir)
mkdir -p "$work/runtime/etc" "$work/runtime/tmp" "$work/runtime/data" "$work/runtime/logs"
cp "$protocols" "$work/runtime/etc/protocols.cfg"

ready="$work/fixture.ports"
fixture_command=(python3 "$fixture" "$ready")
if [[ -n ${XYMONNET_TLS_CERT:-} && -n ${XYMONNET_TLS_KEY:-} ]]; then
	fixture_command+=("$XYMONNET_TLS_CERT" "$XYMONNET_TLS_KEY")
fi
"${fixture_command[@]}" >"$work/runtime/logs/fixture.log" 2>&1 &
fixture_pid=$!
register_cleanup "kill $fixture_pid 2>/dev/null || true"

for ((attempt = 0; attempt < 10000; attempt++)); do
	[[ -s $ready ]] && break
	kill -0 "$fixture_pid" 2>/dev/null || {
		cat "$work/runtime/logs/fixture.log" >&2
		fail "loopback fixture exited before becoming ready"
	}
done
[[ -s $ready ]] || fail "loopback fixture did not become ready"
read -r http_port ssh_port tls_port dns_ready < "$ready"

{
	printf '%s' '127.0.0.1 valgrind.test # noconn'
	printf ' http=plain;http://127.0.0.1:%s/good' "$http_port"
	printf ' http=plainbad;http://127.0.0.1:%s/missing' "$http_port"
	printf ' httphead=headok;http://127.0.0.1:%s/good' "$http_port"
	printf ' httphead=headbad;http://127.0.0.1:%s/missing' "$http_port"
	printf ' httpstatus=statusok;http://127.0.0.1:%s/good;2..;4..' "$http_port"
	printf ' httpstatus=statusbad;http://127.0.0.1:%s/missing;2..;4..' "$http_port"
	printf ' cont=contentok;http://127.0.0.1:%s/good;status=ok' "$http_port"
	printf ' cont=contentbad;http://127.0.0.1:%s/good;status=missing' "$http_port"
	printf ' nocont=absentok;http://127.0.0.1:%s/good;failure' "$http_port"
	printf ' nocont=absentbad;http://127.0.0.1:%s/good;status=ok' "$http_port"
	printf ' type=typeok;http://127.0.0.1:%s/json;application/json' "$http_port"
	printf ' type=typebad;http://127.0.0.1:%s/json;text/plain' "$http_port"
	printf ' post=postok;http://127.0.0.1:%s/form;alpha=one;received:alpha=one' "$http_port"
	printf ' post=postbad;http://127.0.0.1:%s/form;alpha=one;received:missing' "$http_port"
	printf ' nopost=nopostok;http://127.0.0.1:%s/form;alpha=one;failure' "$http_port"
	printf ' nopost=nopostbad;http://127.0.0.1:%s/form;alpha=one;received:alpha=one' "$http_port"
	printf ' soap=soapok;http://127.0.0.1:%s/soap;<request/>;soap-ok' "$http_port"
	printf ' soap=soapbad;http://127.0.0.1:%s/soap;<request/>;missing' "$http_port"
	printf ' apache=http://127.0.0.1:%s/server-status?auto' "$http_port"
	if [[ -n ${XYMONNET_LDAP_PORT:-} ]]; then
		printf ' ldap://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)' "$XYMONNET_LDAP_PORT"
		printf ' ldaps://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)' "$XYMONNET_LDAP_PORT"
	fi
	if [[ $dns_ready = 1 ]]; then
		printf ' dns=A:fixture.xymon.test dig=A:fixture.xymon.test'
		printf ' dns=A:missing.xymon.test dig=A:missing.xymon.test'
	fi
	printf ' ssh:%s !ssh:1 ssh:1 !ssh:%s' "$ssh_port" "$ssh_port"
	printf ' qmtp:%s qmtp:1 ftp:%s ftp:1 smtp:1' "$ssh_port" "$ssh_port"
	if [[ -n $tls_port ]]; then
		printf ' ftps:%s ftps:%s' "$tls_port" "$ssh_port"
	fi
	printf '\n'
	if [[ -n ${XYMONNET_LDAP_PORT:-} ]]; then
		printf '127.0.0.1 ldapfail.test # noconn'
		printf ' ldap://127.0.0.1:%s/dc=missing,dc=test?dc?base?(objectClass=*)\n' "$XYMONNET_LDAP_PORT"
		printf '127.0.0.1 ldaptlsfail.test # noconn'
		printf ' ldaps://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)\n' "$ssh_port"
	fi
	if [[ -n $tls_port ]]; then
		printf '127.0.0.1 certfail.test # noconn ssldays=400:400 ftps:%s\n' "$tls_port"
	fi
} > "$work/runtime/etc/hosts.cfg"

export XYMONHOME="$work/runtime"
export HOSTSCFG="$work/runtime/etc/hosts.cfg"
export XYMONTMP="$work/runtime/tmp"
export XYMONVAR="$work/runtime/data"
export XYMONSERVERLOGS="$work/runtime/logs"

command=("$XYMONNET")
if [[ ${XYMONNET_VALGRIND:-0} = 1 ]]; then
	command -v valgrind >/dev/null 2>&1 || skip "valgrind not found"
	command=(valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all
		--track-origins=yes --errors-for-leak-kinds=definite,indirect,possible
		--error-exitcode=99 --log-file="$work/valgrind.log" "$XYMONNET")
fi

rc=0
"${command[@]}" --no-update >"$work/xymonnet.out" 2>"$work/xymonnet.err" || rc=$?
if ((rc != 0)); then
	cat "$work/xymonnet.out" >&2
	cat "$work/xymonnet.err" >&2
	[[ -f $work/valgrind.log ]] && cat "$work/valgrind.log" >&2
	fail "xymonnet exited with status $rc"
fi

for expected in \
	'valgrind,test.plain green' \
	'valgrind,test.plainbad red' \
	'valgrind,test.headok green' \
	'valgrind,test.headbad red' \
	'valgrind,test.statusok green' \
	'valgrind,test.statusbad red' \
	'valgrind,test.contentok green' \
	'valgrind,test.contentbad red' \
	'valgrind,test.absentok green' \
	'valgrind,test.absentbad red' \
	'valgrind,test.typeok green' \
	'valgrind,test.typebad red' \
	'valgrind,test.postok green' \
	'valgrind,test.postbad red' \
	'valgrind,test.nopostok green' \
	'valgrind,test.nopostbad red' \
	'valgrind,test.soapok green' \
	'valgrind,test.soapbad red' \
	'data valgrind,test.apache' \
	'valgrind,test.ssh green' \
	'valgrind,test.qmtp green' \
	'valgrind,test.qmtp red' \
	'valgrind,test.ftp green' \
	'valgrind,test.ftp red' \
	'valgrind,test.smtp red'
do
	grep -Fq "$expected" "$work/xymonnet.out" || {
		cat "$work/xymonnet.out" >&2
		fail "missing expected result: $expected"
	}
done

if [[ -n $tls_port ]]; then
	for tls_expected in \
		'valgrind,test.ftps green' \
		'valgrind,test.ftps red' \
		'valgrind,test.sslcert green' \
		'certfail,test.sslcert red'
	do
		grep -Fq "$tls_expected" "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "missing expected TLS result: $tls_expected"
		}
	done
fi

if [[ $dns_ready = 1 ]]; then
	[[ $(grep -Fc 'valgrind,test.dns green' "$work/xymonnet.out") = 2 ]] || {
		cat "$work/xymonnet.out" >&2
		cat "$work/xymonnet.err" >&2
		fail "expected successful dns and dig reports"
	}
	[[ $(grep -Fc 'valgrind,test.dns red' "$work/xymonnet.out") = 2 ]] || {
		cat "$work/xymonnet.out" >&2
		cat "$work/xymonnet.err" >&2
		fail "expected failed dns and dig reports"
	}
	grep -Fq 'fixture.xymon.test' "$work/xymonnet.out" || fail "DNS answer is missing"
	grep -Fq 'Name not found' "$work/xymonnet.out" || fail "DNS failure is missing"
fi

if [[ -n ${XYMONNET_LDAP_PORT:-} ]]; then
	for ldap_expected in \
		'valgrind,test.ldap green' \
		'ldapfail,test.ldap red' \
		'ldaptlsfail,test.ldap red' \
		"ldap://127.0.0.1:$XYMONNET_LDAP_PORT/" \
		"ldaps://127.0.0.1:$XYMONNET_LDAP_PORT/"
	do
		grep -Fq "$ldap_expected" "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "missing expected LDAP result: $ldap_expected"
		}
	done
fi

[[ $(grep -Fc 'valgrind,test.ssh green' "$work/xymonnet.out") = 2 ]] ||
	fail "expected successful reports for the positive and reverse SSH checks"
[[ $(grep -Fc 'valgrind,test.ssh red' "$work/xymonnet.out") = 2 ]] ||
	fail "expected failed reports for the positive and reverse SSH checks"

if [[ ${XYMONNET_VALGRIND:-0} = 1 ]]; then
	grep -Eq 'definitely lost: 0 bytes in 0 blocks' "$work/valgrind.log" || fail "Valgrind found definitely lost memory"
	grep -Eq 'indirectly lost: 0 bytes in 0 blocks' "$work/valgrind.log" || fail "Valgrind found indirectly lost memory"
	grep -Eq 'possibly lost: 0 bytes in 0 blocks' "$work/valgrind.log" || fail "Valgrind found possibly lost memory"
	grep -Eq 'ERROR SUMMARY: 0 errors from 0 contexts' "$work/valgrind.log" || fail "Valgrind reported memory errors"
fi

pass "xymonnet loopback network scenarios"