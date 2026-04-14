#!/usr/bin/env bash
# install-workloads.sh — copy *-linux workload binaries into rootfs /root/
# Usage: install-workloads.sh <workloads_output_dir> <rootfs_root_dir>
set -euo pipefail

WORKLOADS_DIR="$1"
ROOTFS_ROOT="$2"

if [ ! -d "$WORKLOADS_DIR" ]; then
    echo "[kernel] no workload output dir found ($WORKLOADS_DIR) — run bbdev workload --build first"
    exit 0
fi

count=0
while IFS= read -r -d '' f; do
    cp "$f" "$ROOTFS_ROOT/"
    echo "[kernel] installed $(basename "$f")"
    count=$((count + 1))
done < <(find "$WORKLOADS_DIR" -name '*-linux' -type f -print0)

echo "[kernel] $count workload binary/binaries installed into /root/"
