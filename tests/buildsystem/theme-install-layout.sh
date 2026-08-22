#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
makefile="$ROOT/xymond/Makefile"
assert_file_exists "$makefile" "xymond Makefile is missing"
assert_file_exists "$ROOT/xymond/themes/classic-blue/theme.conf" "default theme manifest is missing"
assert_file_exists "$ROOT/xymond/themes/classic-grey/theme.conf" "grey theme manifest is missing"
assert_file_exists "$ROOT/xymond/themes/clean/theme.conf" "example theme manifest is missing"

setup_newfiles="$ROOT/build/setup-newfiles"
[ -x "$setup_newfiles" ] || skip "build/setup-newfiles is not built"

install_themes() {
	local stage=$1 staticdir=$2 wwwdir=$3
	make -s -C "$ROOT/xymond" install-themes \
		INSTALLROOT="$stage" \
		INSTALLETCDIR=/etc/xymon \
		INSTALLSTATICWWWDIR="$staticdir" \
		INSTALLWWWDIR="$wwwdir" \
		INSTALLWEBDIR=/usr/lib/xymon/server/web
}

split=$(mktempdir)
mkdir -p "$split/etc/xymon" "$split/usr/share/xymon/menu" \
	"$split/usr/lib/xymon/server/web"
printf 'LOCAL_SERVER_CONFIG\n' >"$split/etc/xymon/xymonserver.cfg"
printf 'LOCAL_MENU_CONFIG\n' >"$split/etc/xymon/xymonmenu.cfg"
printf 'LOCAL_STOCK_CSS\n' >"$split/usr/share/xymon/menu/xymonmenu-blue.css"
printf 'LOCAL_INFO_HEADER\n' >"$split/usr/lib/xymon/server/web/info_header"

install_themes "$split" /usr/share/xymon /var/lib/xymon/www
assert_file_exists "$split/etc/xymon/themes/classic-blue/theme.conf" "default theme was not installed"
assert_file_exists "$split/etc/xymon/themes/classic-grey/theme.conf" "grey theme was not installed"
assert_file_exists "$split/etc/xymon/themes/clean/theme.conf" "manifest was not installed"
assert_contains 'xymonmenu-blue.css' "$(cat "$split/etc/xymon/themes/classic-blue/theme.conf")" "default theme does not select the blue menu"
assert_contains 'xymonmenu-grey.css' "$(cat "$split/etc/xymon/themes/classic-grey/theme.conf")" "grey theme does not select the grey menu"
assert_file_exists "$split/usr/share/xymon/menu/themes/clean/xymonbody.css" "body CSS ignored the split static destination"
assert_file_exists "$split/usr/share/xymon/menu/themes/clean/xymonmenu.css" "menu CSS ignored the split static destination"
assert_equal "LOCAL_SERVER_CONFIG" "$(cat "$split/etc/xymon/xymonserver.cfg")" "theme install changed xymonserver.cfg"
assert_equal "LOCAL_MENU_CONFIG" "$(cat "$split/etc/xymon/xymonmenu.cfg")" "theme install changed xymonmenu.cfg"
assert_equal "LOCAL_STOCK_CSS" "$(cat "$split/usr/share/xymon/menu/xymonmenu-blue.css")" "theme install changed stock CSS"
assert_equal "LOCAL_INFO_HEADER" "$(cat "$split/usr/lib/xymon/server/web/info_header")" "theme install changed a stock template"

printf 'LOCAL_THEME_CSS\n' >"$split/usr/share/xymon/menu/themes/clean/xymonbody.css"
install_themes "$split" /usr/share/xymon /var/lib/xymon/www
assert_equal "LOCAL_THEME_CSS" "$(cat "$split/usr/share/xymon/menu/themes/clean/xymonbody.css")" "theme upgrade replaced a locally modified asset"

combined=$(mktempdir)
install_themes "$combined" /var/lib/xymon/www /var/lib/xymon/www
assert_file_exists "$combined/var/lib/xymon/www/menu/themes/clean/xymonbody.css" "combined static destination was not supported"

catalog=$("$ROOT/build/generate-md5.sh")
assert_contains "themes/classic-blue/theme.conf" "$catalog" "release MD5 catalog omitted the default theme"
assert_contains "themes/classic-grey/theme.conf" "$catalog" "release MD5 catalog omitted the grey theme"
assert_contains "themes/clean/theme.conf" "$catalog" "release MD5 catalog omitted theme manifests"
assert_contains "themes/clean/static/xymonbody.css" "$catalog" "release MD5 catalog omitted theme assets"

pass "theme packages install separately from stock files in combined and split web layouts"
