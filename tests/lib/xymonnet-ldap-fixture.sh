#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by xymonnet-system-container.sh after slapd is installed.

set -euo pipefail

ldap_root=/work/runtime/ldap
ldap_port=389
mkdir -p "$ldap_root/data"

openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
	-subj '/CN=127.0.0.1' -addext 'subjectAltName=IP:127.0.0.1' \
	-keyout "$ldap_root/server.key" -out "$ldap_root/server.crt" \
	>/dev/null 2>&1
chmod 600 "$ldap_root/server.key"

openssl req -newkey rsa:2048 -nodes -subj '/CN=xymonnet-client' \
	-keyout "$ldap_root/client.key" -out "$ldap_root/client.csr" \
	>/dev/null 2>&1
printf '%s\n' 'extendedKeyUsage=clientAuth' > "$ldap_root/client.ext"
openssl x509 -req -days 365 -in "$ldap_root/client.csr" \
	-CA "$ldap_root/server.crt" -CAkey "$ldap_root/server.key" -CAcreateserial \
	-extfile "$ldap_root/client.ext" -out "$ldap_root/client.crt" \
	>/dev/null 2>&1
cat "$ldap_root/client.crt" "$ldap_root/client.key" > "$ldap_root/client.pem"
chmod 600 "$ldap_root/client.key" "$ldap_root/client.pem"

cat > "$ldap_root/slapd.conf" <<EOF
include /etc/ldap/schema/core.schema
include /etc/ldap/schema/cosine.schema
pidfile $ldap_root/slapd.pid
argsfile $ldap_root/slapd.args
modulepath /usr/lib/ldap
moduleload back_mdb
TLSCACertificateFile $ldap_root/server.crt
TLSCertificateFile $ldap_root/server.crt
TLSCertificateKeyFile $ldap_root/server.key

database mdb
maxsize 10485760
suffix "dc=xymon,dc=test"
rootdn "cn=admin,dc=xymon,dc=test"
rootpw fixture-password
directory $ldap_root/data
access to * by * read
EOF

cat > "$ldap_root/fixture.ldif" <<'EOF'
dn: dc=xymon,dc=test
objectClass: top
objectClass: domain
dc: xymon

dn: cn=fixture,dc=xymon,dc=test
objectClass: top
objectClass: organizationalRole
cn: fixture
description: xymonnet LDAP loopback fixture
EOF

slapadd -f "$ldap_root/slapd.conf" -l "$ldap_root/fixture.ldif"
slapd -f "$ldap_root/slapd.conf" -h "ldap://127.0.0.1:$ldap_port/" \
	-d 1 >"$ldap_root/slapd.log" 2>&1 &
ldap_pid=$!

ldap_ready=0
for ((attempt = 0; attempt < 10000; attempt++)); do
	if ldapsearch -x -H "ldap://127.0.0.1:$ldap_port" \
		-b 'dc=xymon,dc=test' -s base '(objectClass=*)' dn \
		>/dev/null 2>&1; then
		ldap_ready=1
		break
	fi
	kill -0 "$ldap_pid" 2>/dev/null || {
		cat "$ldap_root/slapd.log" >&2
		exit 1
	}
done

if ((ldap_ready == 0)); then
	cat "$ldap_root/slapd.log" >&2
	exit 1
fi

LDAPTLS_REQCERT=never ldapsearch -x -ZZ \
	-H "ldap://127.0.0.1:$ldap_port" \
	-b 'dc=xymon,dc=test' -s base '(objectClass=*)' dn \
	>/dev/null

export LDAPTLS_REQCERT=never
export XYMONNET_LDAP_PORT=$ldap_port
export XYMONNET_TLS_CERT=$ldap_root/server.crt
export XYMONNET_TLS_KEY=$ldap_root/server.key
export XYMONNET_CLIENT_CERT=$ldap_root/client.pem