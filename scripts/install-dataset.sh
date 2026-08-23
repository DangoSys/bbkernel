#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "Usage: install-dataset.sh <host_dataset_dir> <rootfs_root_dir> <rel_dest>" >&2
  exit 1
fi

HOST_DIR="$1"
ROOTFS_ROOT="$2"
REL_DEST="$3"

if [ -z "$HOST_DIR" ] || [ -z "$ROOTFS_ROOT" ] || [ -z "$REL_DEST" ]; then
  echo "[kernel] install-dataset: empty argument" >&2
  exit 1
fi
case "$REL_DEST" in
  /*|*".."*)
    echo "[kernel] install-dataset: rel_dest must be relative without ..: $REL_DEST" >&2
    exit 1
    ;;
esac
if [ ! -d "$HOST_DIR" ]; then
  echo "[kernel] dataset dir not found: $HOST_DIR" >&2
  exit 1
fi

DEST="$ROOTFS_ROOT/$REL_DEST"
mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST"
cp -a "$HOST_DIR" "$DEST"
echo "[kernel] installed dataset $HOST_DIR -> /root/$REL_DEST"
