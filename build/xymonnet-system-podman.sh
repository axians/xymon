#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build xymonnet and run its system tests in a container. Defaults to
# Ubuntu; set XYMON_SYSTEM_IMAGE to run against another supported image,
# e.g. XYMON_SYSTEM_IMAGE=rockylinux:10 (or quay.io/rockylinux/rockylinux:10
# if the "rockylinux" short name isn't configured). See
# tests/lib/xymonnet-system-container.sh for which OS families are
# recognized (apt/dnf package sets, OpenLDAP schema/module paths, etc.) --
# it detects the family from /etc/os-release, so no other flag is needed.

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

# Both cache volumes are always mounted regardless of which image is
# selected -- whichever one the container's package manager doesn't use
# just sits there empty, which is simpler than branching on $image here
# too (tests/lib/xymonnet-system-container.sh already does that once,
# based on /etc/os-release, which is the single source of truth).
exec podman run --rm --network private --cap-add=NET_RAW \
	--security-opt label=disable \
	-e "XYMONNET_VALGRIND=${XYMONNET_VALGRIND:-0}" \
	-v "$apt_cache_volume:/var/cache/apt" \
	-v "$apt_lists_volume:/var/lib/apt/lists" \
	-v "$dnf_cache_volume:/var/cache/dnf" \
	-v "$root:/src:ro" "$image" \
	bash /src/tests/lib/xymonnet-system-container.sh