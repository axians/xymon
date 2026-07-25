#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build xymonnet and run its loopback regression under Valgrind in Ubuntu.

set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
image=${XYMON_VALGRIND_IMAGE:-ubuntu:24.04}

command -v podman >/dev/null 2>&1 || {
	printf 'podman is required\n' >&2
	exit 77
}

exec podman run --rm --network private --security-opt label=disable \
	-e "XYMONNET_VALGRIND=${XYMONNET_VALGRIND:-1}" \
	-v "$root:/src:ro" "$image" \
	bash /src/tests/lib/xymonnet-valgrind-container.sh