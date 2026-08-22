#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
helper="$ROOT/xymond/xymon-theme.DIST"
assert_file_exists "$helper" "theme helper source is missing"

work=$(mktempdir)
etcdir="$work/etc"
themes="$etcdir/themes"
active="$etcdir/xymonserver.d/90-theme.cfg"
mkdir -p "$themes/classic-blue" "$themes/classic-grey" "$themes/clean" "$themes/contrast"

cat >"$themes/classic-blue/theme.conf" <<'EOF'
XYMONBODYMENUCSS="$XYMONMENUSKIN/xymonmenu-blue.css"
EOF
cat >"$themes/classic-grey/theme.conf" <<'EOF'
XYMONBODYMENUCSS="$XYMONMENUSKIN/xymonmenu-grey.css"
EOF
cat >"$themes/clean/theme.conf" <<'EOF'
XYMONSKIN="$XYMONSERVERWWWURL/menu/themes/clean"
XYMONMENUSKIN="$XYMONSERVERWWWURL/menu/themes/clean"
XYMONBODYCSS="$XYMONSKIN/xymonbody.css"
XYMONBODYMENUCSS="$XYMONMENUSKIN/xymonmenu.css"
EOF
cat >"$themes/contrast/theme.conf" <<'EOF'
XYMONBODYCSS="$XYMONSERVERWWWURL/menu/themes/contrast/xymonbody.css"
XYMONLOGO="Contrast"
EOF

theme() {
	XYMON_THEME_ETCDIR="$etcdir" sh "$helper" "$@"
}

assert_equal "classic-blue" "$(theme current)" "an installation with no selection does not report the default theme"
assert_equal $'classic-blue\nclassic-grey\nclean\ncontrast' "$(theme list)" "installed themes are listed"

assert_equal "clean" "$(theme activate clean)" "activation reports the selected theme"
assert_file_exists "$active" "activation did not create its drop-in"
assert_equal "clean" "$(theme current)" "current does not report the active theme"
active_text=$(cat "$active")
assert_contains "# Managed by xymon-theme" "$active_text" "drop-in has no ownership marker"
assert_contains "XYMONBODYCSS=\"\$XYMONSKIN/xymonbody.css\"" "$active_text" "manifest was not preserved"

theme activate contrast >/dev/null
assert_equal "contrast" "$(theme current)" "a second activation did not replace the first"
theme disable
assert_equal "classic-blue" "$(theme current)" "disable did not restore the default theme"

cat >"$themes/contrast/theme.conf" <<'EOF'
XYMONSERVERIP="203.0.113.7"
EOF
if theme activate contrast >"$work/out" 2>"$work/err"; then
	fail "a theme could set a non-presentation variable"
fi
assert_contains "unsupported variable XYMONSERVERIP" "$(cat "$work/err")" "rejection did not identify the unsafe variable"

if theme activate ../clean >"$work/out" 2>"$work/err"; then
	fail "a path traversal theme ID was accepted"
fi
assert_contains "invalid theme ID" "$(cat "$work/err")" "path traversal rejection was unclear"

mkdir -p "$(dirname "$active")"
printf 'XYMONBODYCSS="/local/admin.css"\n' >"$active"
if theme activate clean >"$work/out" 2>"$work/err"; then
	fail "activation replaced an administrator-owned drop-in"
fi
assert_contains "refusing to replace unmanaged" "$(cat "$work/err")" "unmanaged activation refusal was unclear"
if theme disable >"$work/out" 2>"$work/err"; then
	fail "disable removed an administrator-owned drop-in"
fi
assert_contains "refusing to remove unmanaged" "$(cat "$work/err")" "unmanaged disable refusal was unclear"
assert_contains "/local/admin.css" "$(cat "$active")" "administrator-owned drop-in was changed"

pass "themes switch atomically without modifying stock or administrator-owned configuration"
