#!/usr/bin/env bash

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)

[ -f "$ROOT/xymonproxy/xymon-http-gateway.py" ] || \
	skip "xymon-http-gateway.py absent"
command -v python3 >/dev/null 2>&1 || skip "python3 not found"

cd "$ROOT"
python3 -m unittest -v tests/server/test_xymon_http_gateway.py || \
	fail "Xymon HTTP gateway integration tests failed"

pass "Xymon HTTP gateway forwards allowed messages and rejects unsafe requests"
