#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build xymonnet and run its system tests in Ubuntu.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
image=${XYMON_SYSTEM_IMAGE:-ubuntu:24.04}
apt_cache_volume=${XYMON_SYSTEM_APT_CACHE_VOLUME:-xymonnet-system-apt-cache}
apt_lists_volume=${XYMON_SYSTEM_APT_LISTS_VOLUME:-xymonnet-system-apt-lists}

command -v podman >/dev/null 2>&1 || {
	printf 'podman is required\n' >&2
	exit 77
}

exec podman run --rm --network private --cap-add=NET_RAW \
	--security-opt label=disable \
	-e "XYMONNET_VALGRIND=${XYMONNET_VALGRIND:-0}" \
	-v "$apt_cache_volume:/var/cache/apt" \
	-v "$apt_lists_volume:/var/lib/apt/lists" \
	-v "$root:/src:ro" "$image" \
	bash /src/tests/lib/xymonnet-system-container.sh