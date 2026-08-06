#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Exercise the full xymonnet binary against deterministic loopback network
# fixtures. Set XYMONNET_VALGRIND=1 to run the same scenario under Memcheck.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/../../lib/assert.sh"

require_bin XYMONNET xymonnet/xymonnet
command -v python3 >/dev/null 2>&1 || skip "python3 not found"

root=$(find_root)
fixture="$root/tests/fixtures/xymonnet-loopback.py"
protocols="$root/xymonnet/protocols.cfg"
assert_file_exists "$fixture"
assert_file_exists "$protocols"
ssl_dialect_ready=0
grep -Fq 'resolve_ssl_dialect_service' "$root/xymonnet/xymonnet.c" && ssl_dialect_ready=1
dns_aggregate_ready=0
grep -Fq 'aggregate_open' "$root/xymonnet/xymonnet.c" && dns_aggregate_ready=1
dns_extra_ready=0
grep -Fq 'dns_decode_content_pattern' "$root/xymonnet/dns.c" && dns_extra_ready=1

work=$(mktempdir)
mkdir -p "$work/runtime/etc" "$work/runtime/tmp" "$work/runtime/data" \
	"$work/runtime/logs" "$work/runtime/certs"
cp "$protocols" "$work/runtime/etc/protocols.cfg"
cat > "$work/runtime/etc/netrc" <<'EOF'
machine netrc.test login fixture password password
EOF
chmod 600 "$work/runtime/etc/netrc"
if [[ -n ${XYMONNET_CLIENT_CERT:-} ]]; then
	cp "$XYMONNET_CLIENT_CERT" "$work/runtime/certs/client.pem"
	chmod 600 "$work/runtime/certs/client.pem"
fi

ready="$work/fixture.ports"
fixture_command=(python3 "$fixture" "$ready")
if [[ -n ${XYMONNET_TLS_CERT:-} && -n ${XYMONNET_TLS_KEY:-} ]]; then
	fixture_command+=("$XYMONNET_TLS_CERT" "$XYMONNET_TLS_KEY")
fi
"${fixture_command[@]}" >"$work/runtime/logs/fixture.log" 2>&1 &
fixture_pid=$!
register_cleanup "kill $fixture_pid 2>/dev/null || true"

for ((attempt = 0; attempt < 1000; attempt++)); do
	[[ -s $ready ]] && break
	kill -0 "$fixture_pid" 2>/dev/null || {
		cat "$work/runtime/logs/fixture.log" >&2
		fail "loopback fixture exited before becoming ready"
	}
	sleep 0.01
done
[[ -s $ready ]] || fail "loopback fixture did not become ready"
read -r http_port ssh_port bad_banner_port ftp_port telnet_port tls_port \
	https_port mtls_port dns_ready ntp_ready empty_port tls12_port < "$ready"

{
	printf '%s' '127.0.0.1 system.test #'
	printf ' http=plain;http://127.0.0.1:%s/good' "$http_port"
	printf ' http=plainbad;http://127.0.0.1:%s/missing' "$http_port"
	printf ' http=redirect;http://127.0.0.1:%s/redirect' "$http_port"
	printf ' http=authok;http://fixture:password@127.0.0.1:%s/auth' "$http_port"
	printf ' http=authbad;http://fixture:wrong@127.0.0.1:%s/auth' "$http_port"
	printf ' http=netrcok;http://netrc.test:%s=127.0.0.1/auth' "$http_port"
	printf ' http=netrcbad;http://missing-netrc.test:%s=127.0.0.1/auth' "$http_port"
	printf ' httphead=headok;http://127.0.0.1:%s/good' "$http_port"
	printf ' httphead=headbad;http://127.0.0.1:%s/missing' "$http_port"
	printf ' httpstatus=statusok;http://127.0.0.1:%s/good;2..;4..' "$http_port"
	printf ' httpstatus=statusbad;http://127.0.0.1:%s/missing;2..;4..' "$http_port"
	printf ' httpstatus=statusaltok;http://127.0.0.1:%s/redirect;2..|302;4..|5..' "$http_port"
	printf ' httpstatus=statuserr;http://127.0.0.1:%s/error;2..;4..|5..' "$http_port"
	printf ' httpstatus=statusokonly;http://127.0.0.1:%s/good;2..;' "$http_port"
	printf ' httpstatus=statusbadonly;http://127.0.0.1:%s/missing;;4..' "$http_port"
	printf ' httpstatus=statusunreach;http://127.0.0.1:%s/nope;2..;999' "$empty_port"
	printf ' cont=http10ok;http10://127.0.0.1:%s/httpversion;HTTP/1.0' "$http_port"
	printf ' cont=http11ok;http11://127.0.0.1:%s/httpversion;HTTP/1.1' "$http_port"
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
	printf ' nosoap=nosoapok;http://127.0.0.1:%s/soap;<request/>;missing' "$http_port"
	printf ' nosoap=nosoapbad;http://127.0.0.1:%s/soap;<request/>;soap-ok' "$http_port"
	printf ' apache=http://127.0.0.1:%s/server-status?auto' "$http_port"
	if [[ -n ${XYMONNET_LDAP_PORT:-} ]]; then
		printf ' ldap://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)' "$XYMONNET_LDAP_PORT"
		printf ' ldaps://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)' "$XYMONNET_LDAP_PORT"
		if [[ $ssl_dialect_ready = 1 ]]; then
			printf ' ldapsd://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)' "$XYMONNET_LDAP_PORT"
			printf ' ldapst://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)' "$XYMONNET_LDAP_PORT"
		fi
	fi
	if [[ $dns_ready = 1 ]]; then
		printf ' dns=A:fixture.xymon.test dig=A:fixture.xymon.test'
		printf ' dns=A:missing.xymon.test dig=A:missing.xymon.test'
	fi
	if [[ $ntp_ready = 1 ]]; then
		printf ' ntp'
	fi
	printf ' ssh:%s !ssh:1 ssh:1 !ssh:%s' "$ssh_port" "$ssh_port"
	printf ' qmtp:%s qmtp:1 ftp:%s ftp:1 smtp:1' "$ssh_port" "$ftp_port"
	printf ' telnet:%s' "$telnet_port"
	if [[ $tls_port != 0 ]]; then
		printf ' ftps:%s ftps:%s' "$tls_port" "$ssh_port"
		if [[ $ssl_dialect_ready = 1 ]]; then
			printf ' ftpsd:%s' "$tls_port"
			printf ' ftpst:%s' "$tls_port"
		fi
	fi
	if [[ $https_port != 0 ]]; then
		printf ' http=httpsok;https://127.0.0.1:%s/good' "$https_port"
		# Scheme-suffix "dialects" from hosts.cfg(5): SSLv2/SSLv3 ("2"/"3") are
		# compiled out on modern OpenSSL, so forcing them is a silent no-op --
		# xymonnet falls through to a normal default-negotiated TLS connection.
		printf ' http=sslv2noop;https2://127.0.0.1:%s/good' "$https_port"
		printf ' http=sslv3noop;https3://127.0.0.1:%s/good' "$https_port"
		# TLSv1.0/1.1 ("t"/"a"/"b") are disabled by OpenSSL 3.x by default, so
		# forcing them can never complete a handshake in this environment.
		printf ' http=tls10fail;httpst://127.0.0.1:%s/good' "$https_port"
		printf ' http=tls10altfail;httpsa://127.0.0.1:%s/good' "$https_port"
		printf ' http=tls11fail;httpsb://127.0.0.1:%s/good' "$https_port"
		printf ' http=tls13ok;httpsd://127.0.0.1:%s/good' "$https_port"
		# Cipher-strength suffixes: "HIGH" matches broadly (succeeds); "MEDIUM"
		# matches nothing on modern OpenSSL, so the configured restriction is
		# rejected and the test reports an SSL error.
		printf ' http=cipherhighok;httpsh://127.0.0.1:%s/good' "$https_port"
		printf ' http=ciphermediumfail;httpsm://127.0.0.1:%s/good' "$https_port"
	fi
	if [[ $tls12_port != 0 ]]; then
		# A TLSv1.2-only listener proves version forcing is a real constraint,
		# not just "some default TLS version happened to work".
		printf ' http=tls12ok;httpsc://127.0.0.1:%s/good' "$tls12_port"
		printf ' http=tls13mismatch;httpsd://127.0.0.1:%s/good' "$tls12_port"
	fi
	if [[ $mtls_port != 0 ]]; then
		printf ' http=certauthok;https://CERT:client.pem@127.0.0.1:%s/good' "$mtls_port"
		printf ' http=certauthbad;https://127.0.0.1:%s/good' "$mtls_port"
	fi
	printf '\n'
	printf '127.0.0.1 bannerbad.test # noconn ssh:%s\n' "$bad_banner_port"
	printf '192.0.2.1 pingfail.test # ?conn\n'
	printf '127.0.0.1 pingreverse.test # !conn\n'
	if [[ $ntp_ready = 1 ]]; then
		printf '192.0.2.1 ntpfail.test # noconn ntp\n'
	fi
	if [[ $dns_ready = 1 && $dns_extra_ready = 1 ]]; then
		printf '127.0.0.1 dnscontentok.test # noconn dns=A:fixture.xymon.test;127[.]0[.]0[.]1\n'
		printf '127.0.0.1 dnscontentfail.test # noconn dns=A:fixture.xymon.test;192[.]0[.]2[.]1\n'
		printf '%s' '127.0.0.1 dnsrecordtypes.test # noconn'
		printf '%s' ' dns=A:fixture.xymon.test,AAAA:aaaa.fixture.xymon.test'
		printf '%s' ',CNAME:alias.fixture.xymon.test,MX:mx.fixture.xymon.test'
		printf '%s' ',NS:ns.fixture.xymon.test,PTR:1.0.0.127.in-addr.arpa'
		printf '%s' ',SOA:soa.fixture.xymon.test,SRV:_service._tcp.fixture.xymon.test'
		printf '%s\n' ',TXT:txt.fixture.xymon.test'
		printf '127.0.0.1 dnsrecordfail.test # noconn dns=AAAA:missing.fixture.xymon.test\n'
	fi
	if [[ -n ${XYMONNET_LDAP_PORT:-} ]]; then
		printf '127.0.0.1 ldapfail.test # noconn'
		printf ' ldap://127.0.0.1:%s/dc=missing,dc=test?dc?base?(objectClass=*)\n' "$XYMONNET_LDAP_PORT"
		printf '127.0.0.1 ldaptlsfail.test # noconn'
		printf ' ldaps://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)\n' "$ssh_port"
		printf '127.0.0.1 ldapauth.test # noconn ldaplogin=cn=admin,dc=xymon,dc=test:fixture-password'
		printf ' ldap://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)\n' "$XYMONNET_LDAP_PORT"
		printf '127.0.0.1 ldapauthfail.test # noconn ldaplogin=cn=admin,dc=xymon,dc=test:wrong-password'
		printf ' ldap://127.0.0.1:%s/dc=xymon,dc=test?dc?base?(objectClass=*)\n' "$XYMONNET_LDAP_PORT"
	fi
	if [[ $tls_port != 0 ]]; then
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
	command=(valgrind --tool=memcheck --leak-check=full
		--show-leak-kinds=definite,indirect,possible
		--track-origins=yes --errors-for-leak-kinds=definite,indirect,possible
		--error-exitcode=99 --log-file="$work/valgrind.log" "$XYMONNET")
fi

rc=0
"${command[@]}" --no-update --checkresponse >"$work/xymonnet.out" 2>"$work/xymonnet.err" || rc=$?
if ((rc != 0)); then
	cat "$work/xymonnet.out" >&2
	cat "$work/xymonnet.err" >&2
	[[ -f $work/valgrind.log ]] && cat "$work/valgrind.log" >&2
	fail "xymonnet exited with status $rc"
fi

for expected in \
	'system,test.plain green' \
	'system,test.plainbad red' \
	'system,test.redirect green' \
	'system,test.authok green' \
	'system,test.authbad red' \
	'system,test.netrcok green' \
	'system,test.netrcbad red' \
	'system,test.headok green' \
	'system,test.headbad red' \
	'system,test.statusok green' \
	'system,test.statusbad red' \
	'system,test.statusaltok green' \
	'system,test.statuserr red' \
	'system,test.statusokonly green' \
	'system,test.statusbadonly red' \
	'system,test.statusunreach red' \
	'system,test.http10ok green' \
	'system,test.http11ok green' \
	'system,test.contentok green' \
	'system,test.contentbad red' \
	'system,test.absentok green' \
	'system,test.absentbad red' \
	'system,test.typeok green' \
	'system,test.typebad red' \
	'system,test.postok green' \
	'system,test.postbad red' \
	'system,test.nopostok green' \
	'system,test.nopostbad red' \
	'system,test.soapok green' \
	'system,test.soapbad red' \
	'system,test.nosoapok green' \
	'system,test.nosoapbad red' \
	'data system,test.apache' \
	'system,test.ssh green' \
	'system,test.qmtp green' \
	'system,test.qmtp red' \
	'system,test.ftp green' \
	'system,test.ftp red' \
	'system,test.smtp red' \
	'system,test.telnet green' \
	'bannerbad,test.ssh yellow' \
	'system,test.conn green' \
	'pingfail,test.conn clear' \
	'pingreverse,test.conn red'
do
	grep -Fq "$expected" "$work/xymonnet.out" || {
		cat "$work/xymonnet.out" >&2
		fail "missing expected result: $expected"
	}
done

grep -Fq 'xymonnet telnet login:' "$work/xymonnet.out" || {
	cat "$work/xymonnet.out" >&2
	fail "telnet negotiation did not expose the fixture banner"
}

if [[ $tls_port != 0 ]]; then
	for tls_expected in \
		'system,test.ftps green' \
		'system,test.ftps red' \
		'system,test.sslcert green' \
		'certfail,test.sslcert red'
	do
		grep -Fq "$tls_expected" "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "missing expected TLS result: $tls_expected"
		}
	done

	if [[ $ssl_dialect_ready = 1 ]]; then
		[[ $(grep -Fc 'system,test.ftps green' "$work/xymonnet.out") = 2 ]] || {
			cat "$work/xymonnet.out" >&2
			fail "expected successful plain and TLSv1.3-dialect ftps reports"
		}
		[[ $(grep -Fc 'system,test.ftps red' "$work/xymonnet.out") = 2 ]] || {
			cat "$work/xymonnet.out" >&2
			fail "expected failed plain and TLSv1.0-dialect ftps reports"
		}
	fi

fi

if [[ $https_port != 0 ]]; then
	grep -Fq 'system,test.httpsok green' "$work/xymonnet.out" || {
		cat "$work/xymonnet.out" >&2
		cat "$work/xymonnet.err" >&2
		fail "missing successful HTTPS result"
	}
	for scheme_expected in \
		'system,test.sslv2noop green' \
		'system,test.sslv3noop green' \
		'system,test.tls10fail red' \
		'system,test.tls10altfail red' \
		'system,test.tls11fail red' \
		'system,test.tls13ok green' \
		'system,test.cipherhighok green' \
		'system,test.ciphermediumfail red'
	do
		grep -Fq "$scheme_expected" "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "missing expected SSL/HTTP scheme-suffix result: $scheme_expected"
		}
	done
fi

if [[ $tls12_port != 0 ]]; then
	for tls12_expected in \
		'system,test.tls12ok green' \
		'system,test.tls13mismatch red'
	do
		grep -Fq "$tls12_expected" "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "missing expected TLS-version-pinned result: $tls12_expected"
		}
	done
fi

if [[ $mtls_port != 0 ]]; then
	for certauth_expected in \
		'system,test.certauthok green' \
		'system,test.certauthbad red'
	do
		grep -Fq "$certauth_expected" "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "missing client-certificate result: $certauth_expected"
		}
	done
fi

if [[ $ntp_ready = 1 ]]; then
	for ntp_expected in \
		'system,test.ntp green' \
		'ntpfail,test.ntp red' \
		'NTP server 127.0.0.1 is synchronised'
	do
		grep -Fq "$ntp_expected" "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "missing expected NTP result: $ntp_expected"
		}
	done
fi

if [[ $dns_ready = 1 ]]; then
	if [[ $dns_aggregate_ready = 1 ]]; then
		[[ $(grep -Fc 'system,test.dns green' "$work/xymonnet.out") = 0 ]] || {
			cat "$work/xymonnet.out" >&2
			fail "expected no green status when an aggregated DNS lookup fails"
		}
		[[ $(grep -Fc 'system,test.dns red' "$work/xymonnet.out") = 1 ]] || {
			cat "$work/xymonnet.out" >&2
			fail "expected one worst-result aggregate DNS status"
		}
	else
		[[ $(grep -Fc 'system,test.dns green' "$work/xymonnet.out") = 2 ]] || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "expected successful dns and dig reports"
		}
		[[ $(grep -Fc 'system,test.dns red' "$work/xymonnet.out") = 2 ]] || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "expected failed dns and dig reports"
		}
	fi
	grep -Fq 'fixture.xymon.test' "$work/xymonnet.out" || fail "DNS answer is missing"
	grep -Fq 'Name not found' "$work/xymonnet.out" || fail "DNS failure is missing"
	if [[ $dns_aggregate_ready = 1 ]]; then
		for dns_testspec in \
			'*** dns=A:fixture.xymon.test ***' \
			'*** dig=A:fixture.xymon.test ***' \
			'*** dns=A:missing.xymon.test ***' \
			'*** dig=A:missing.xymon.test ***'
		do
			[[ $(grep -Fc -- "$dns_testspec" "$work/xymonnet.out") = 1 ]] || {
				cat "$work/xymonnet.out" >&2
				fail "aggregate DNS report did not retain one subtest: $dns_testspec"
			}
		done
	fi
	if [[ $dns_extra_ready = 1 ]]; then
		grep -Fq 'dnscontentok,test.dns green' "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			fail "matching DNS response content did not pass"
		}
		grep -Fq 'dnscontentfail,test.dns red' "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			fail "mismatching DNS response content did not fail"
		}
		grep -Fq 'dnsrecordtypes,test.dns green' "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			fail "one or more supported DNS record types did not pass"
		}
		grep -Fq 'dnsrecordfail,test.dns red' "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			fail "missing DNS record did not fail"
		}
		for dns_answer in \
			'127.0.0.1' \
			'2001:db8::1' \
			'canonical.fixture.xymon.test' \
			'mail.fixture.xymon.test' \
			'ns1.fixture.xymon.test' \
			'ptr-target.fixture.xymon.test' \
			'hostmaster.fixture.xymon.test' \
			'service.fixture.xymon.test' \
			'verification=ready'
		do
			grep -Fq "$dns_answer" "$work/xymonnet.out" || {
				cat "$work/xymonnet.out" >&2
				fail "DNS record-type answer is missing: $dns_answer"
			}
		done
	fi
fi

if [[ -n ${XYMONNET_LDAP_PORT:-} ]]; then
	for ldap_expected in \
		'ldapfail,test.ldap red' \
		'ldaptlsfail,test.ldap red' \
		'ldapauth,test.ldap green' \
		'ldapauthfail,test.ldap red' \
		"ldap://127.0.0.1:$XYMONNET_LDAP_PORT/" \
		"ldaps://127.0.0.1:$XYMONNET_LDAP_PORT/"
	do
		grep -Fq "$ldap_expected" "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			cat "$work/xymonnet.err" >&2
			fail "missing expected LDAP result: $ldap_expected"
		}
	done

	if [[ $ssl_dialect_ready = 1 ]]; then
		for ldap_dialect_expected in \
			"ldapsd://127.0.0.1:$XYMONNET_LDAP_PORT/" \
			"ldapst://127.0.0.1:$XYMONNET_LDAP_PORT/"
		do
			grep -Fq "$ldap_dialect_expected" "$work/xymonnet.out" || {
				cat "$work/xymonnet.out" >&2
				fail "missing expected LDAP dialect result: $ldap_dialect_expected"
			}
		done
		grep -Fq 'system,test.ldap red' "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			fail "expected ldapst TLSv1.0 failure to make the LDAP aggregate red"
		}
	else
		grep -Fq 'system,test.ldap green' "$work/xymonnet.out" || {
			cat "$work/xymonnet.out" >&2
			fail "expected successful LDAP and LDAPS aggregate"
		}
	fi
fi

[[ $(grep -Fc 'system,test.ssh green' "$work/xymonnet.out") = 2 ]] ||
	fail "expected successful reports for the positive and reverse SSH checks"
[[ $(grep -Fc 'system,test.ssh red' "$work/xymonnet.out") = 2 ]] ||
	fail "expected failed reports for the positive and reverse SSH checks"

if [[ ${XYMONNET_VALGRIND:-0} = 1 ]]; then
	grep -Eq 'definitely lost: 0 bytes in 0 blocks' "$work/valgrind.log" || fail "Valgrind found definitely lost memory"
	grep -Eq 'indirectly lost: 0 bytes in 0 blocks' "$work/valgrind.log" || fail "Valgrind found indirectly lost memory"
	grep -Eq 'possibly lost: 0 bytes in 0 blocks' "$work/valgrind.log" || fail "Valgrind found possibly lost memory"
	grep -Eq 'ERROR SUMMARY: 0 errors from 0 contexts' "$work/valgrind.log" || fail "Valgrind reported memory errors"
fi

pass "xymonnet loopback network scenarios"