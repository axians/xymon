#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/server/xymongen-cli-filters.sh
#
# Behavioural test of two xymongen command-line filters from xymongen.1's
# "COLUMN SELECTION OPTIONS": --nongreen-colors and --ignorecolumns. Unlike
# every other test in this area, these are xymongen's own CLI flags, not
# hosts.cfg directives -- the same board dump and hosts.cfg is rendered
# multiple times with different flags to isolate each flag's effect.
#
# Two hosts:
#   red-host.example.com     # conn cpu     (conn green, cpu red)
#   yellow-host.example.com  # conn disk    (conn green, disk yellow)

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

require_bin XYMONGEN xymongen/xymongen

work=$(mktempdir)
mkdir -p "$work"/ack "$work"/hist "$work"/histlogs "$work"/home/web \
	"$work"/rawstatus "$work"/notes "$work"/rep "$work"/tmp "$work"/var "$work"/web
for f in stdnormal_header stdnormal_footer stdnongreen_header stdnongreen_footer \
	stdcritical_header stdcritical_footer; do
	: > "$work/home/web/$f"
done

cat > "$work/hosts.cfg" <<'EOF'
127.0.0.1 red-host.example.com    # conn cpu
127.0.0.1 yellow-host.example.com # conn disk
EOF

cat > "$work/board.dump" <<'EOF'
red-host.example.com|conn|green|||||||127.0.0.1|-1|OK
red-host.example.com|cpu|red|||||||127.0.0.1|-1|Load high
red-host.example.com|info|green|||||||127.0.0.1|-1|Host info
red-host.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
yellow-host.example.com|conn|green|||||||127.0.0.1|-1|OK
yellow-host.example.com|disk|yellow|||||||127.0.0.1|-1|Getting full
yellow-host.example.com|info|green|||||||127.0.0.1|-1|Host info
yellow-host.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
EOF

export XYMONACKDIR="$work/ack"
export XYMONHISTDIR="$work/hist"
export XYMONHISTLOGS="$work/histlogs"
export XYMONHOME="$work/home"
export HOSTSCFG="$work/hosts.cfg"
export XYMONRAWSTATUSDIR="$work/rawstatus"
export XYMONLOGSTATUS=DYNAMIC
export XYMONNOTESDIR="$work/notes"
export XYMONREPDIR="$work/rep"
export XYMONREPURL="/rep"
export XYMONSKIN=default
export XYMONTMP="$work/tmp"
export XYMONVAR="$work/var"
export XYMONWEB="$work/web"
export XYMONWWWDIR="$work/web"
export XYMONWEBHOST="http://localhost"
export XYMONWEBHOSTURL=""
export CGIBINURL="/cgi-bin"
export DOTHEIGHT=16
export DOTWIDTH=16
export MACHINE=localhost
export MACHINEADDR=127.0.0.1
export XYMONPAGECOLFONT=""
export XYMONPAGELOCAL=""
export XYMONPAGESUBLOCAL=""
export XYMONPAGEREMOTE=""
export XYMONPAGEROWFONT=""
export XYMONPAGETITLE="Test"
export PURPLEDELAY=0
export BOARDDUMP="$work/board.dump"

run_xymongen() {
	"$XYMONGEN" "$@" >"$work/xymongen.out" 2>"$work/xymongen.err" || {
		cat "$work/xymongen.out" >&2
		cat "$work/xymongen.err" >&2
		fail "xymongen $* exited non-zero"
	}
}

# --nongreen-colors=red: only red counts as "nongreen" now, so the yellow
# host must be dropped from the page entirely, same as a fully-green host
# would be by default.
run_xymongen --nongreen-colors=red
nongreen=$(cat "$work/web/nongreen.html")
assert_contains 'red-host.example.com' "$nongreen" \
	"--nongreen-colors=red must still include the red host"
assert_not_contains 'yellow-host.example.com' "$nongreen" \
	"--nongreen-colors=red must exclude the yellow host -- yellow is no longer a nongreen color"

# --ignorecolumns=cpu: the cpu column must vanish from the main page
# entirely, for every host, not just stop being red.
run_xymongen --ignorecolumns=cpu
main=$(cat "$work/web/xymon.html")
assert_not_contains 'columndoc.sh?cpu' "$main" "--ignorecolumns=cpu must remove the cpu column header"
assert_not_contains 'ALT="cpu:red:"' "$main" "--ignorecolumns=cpu must remove the cpu status indicator too"
assert_contains 'columndoc.sh?disk' "$main" "--ignorecolumns=cpu must leave unrelated columns alone"

pass "xymongen --nongreen-colors and --ignorecolumns filter correctly"
