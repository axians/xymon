#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build a full Xymon server tree and run the regression catalog
# (./tests/testsuite, which includes every tests/server/xymongen-*.sh test)
# in a container. Defaults to Ubuntu; set XYMON_SYSTEM_IMAGE to run against
# another supported image, e.g. XYMON_SYSTEM_IMAGE=rockylinux:10 (or
# quay.io/rockylinux/rockylinux:10 if the "rockylinux" short name isn't
# configured). See tests/lib/xymongen-container.sh for which OS families are
# recognized -- it detects the family from /etc/os-release, so no other flag
# is needed.
#
# Unlike build/xymonnet-system-podman.sh, this runs no network fixtures --
# it is a plain clean-room build + regression run, useful for verifying the
# tree builds and tests pass on a distro other than your host's without
# creating a real "xymon" system user (see tests/lib/xymongen-container.sh
# for how it avoids that).

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
image=${XYMON_SYSTEM_IMAGE:-ubuntu:24.04}
apt_cache_volume=${XYMON_SYSTEM_APT_CACHE_VOLUME:-xymon-system-apt-cache}
apt_lists_volume=${XYMON_SYSTEM_APT_LISTS_VOLUME:-xymon-system-apt-lists}
dnf_cache_volume=${XYMON_SYSTEM_DNF_CACHE_VOLUME:-xymon-system-dnf-cache}

command -v podman >/dev/null 2>&1 || {
	printf 'podman is required\n' >&2
	exit 77
}

# Same cache volumes as build/xymonnet-system-podman.sh -- both containers
# pull an overlapping package set (build-essential/gcc, openssl, pcre2,
# tirpc, ...), so sharing the cache is a genuine win, not just naming reuse.
exec podman run --rm \
	--security-opt label=disable \
	-v "$apt_cache_volume:/var/cache/apt" \
	-v "$apt_lists_volume:/var/lib/apt/lists" \
	-v "$dnf_cache_volume:/var/cache/dnf" \
	-v "$root:/src:ro" "$image" \
	bash /src/tests/lib/xymongen-container.sh
