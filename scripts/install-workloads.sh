#!/usr/bin/env bash
# install-workloads.sh — copy *-linux workload binaries into rootfs /root/
# Usage:
#   install-workloads.sh <workloads_output_dir> <rootfs_root_dir>
#   install-workloads.sh <workloads_output_dir> <rootfs_root_dir> <allowlist_file>
#
# With allowlist: every stem must resolve to exactly one file, else fail.
# Without allowlist: install every *-linux under workloads_output_dir; zero is an error.
set -euo pipefail

WORKLOADS_DIR="$1"
ROOTFS_ROOT="$2"
ALLOWLIST="${3:-}"

if [ ! -d "$WORKLOADS_DIR" ]; then
    echo "[kernel] ERROR: workload output dir not found: $WORKLOADS_DIR" >&2
    echo "[kernel] run bbdev workload --build first" >&2
    exit 1
fi

if [ ! -d "$ROOTFS_ROOT" ]; then
    echo "[kernel] ERROR: rootfs root dir not found: $ROOTFS_ROOT" >&2
    exit 1
fi

count=0

if [ -n "$ALLOWLIST" ]; then
    if [ ! -f "$ALLOWLIST" ]; then
        echo "[kernel] ERROR: allowlist not found: $ALLOWLIST" >&2
        exit 1
    fi
    while IFS= read -r stem || [ -n "$stem" ]; do
        stem=$(echo "$stem" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
        [ -n "$stem" ] || continue
        mapfile -t hits < <(find "$WORKLOADS_DIR" -type f -name "$stem" | sort)
        if [ "${#hits[@]}" -eq 0 ]; then
            echo "[kernel] ERROR: workload not found for stem: $stem" >&2
            exit 1
        fi
        if [ "${#hits[@]}" -ne 1 ]; then
            echo "[kernel] ERROR: duplicate workload paths for stem: $stem" >&2
            printf '[kernel]   %s\n' "${hits[@]}" >&2
            exit 1
        fi
        cp "${hits[0]}" "$ROOTFS_ROOT/"
        echo "[kernel] installed $stem"
        count=$((count + 1))
    done < "$ALLOWLIST"
else
    while IFS= read -r -d '' f; do
        cp "$f" "$ROOTFS_ROOT/"
        echo "[kernel] installed $(basename "$f")"
        count=$((count + 1))
    done < <(find "$WORKLOADS_DIR" -name '*-linux' -type f -print0 | sort -z)
fi

if [ "$count" -eq 0 ]; then
    echo "[kernel] ERROR: no workload binaries installed into /root/" >&2
    exit 1
fi

echo "[kernel] $count workload binary/binaries installed into /root/"
