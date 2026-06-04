#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "Usage: install-model.sh <model_output_dir> <run_binary> <rootfs_root_dir>" >&2
  exit 1
fi

MODEL_DIR="$1"
RUN_BINARY="$2"
ROOTFS_ROOT="$3"

if [ ! -d "$MODEL_DIR" ]; then
  echo "[kernel] model output dir not found: $MODEL_DIR" >&2
  exit 1
fi

if [ ! -f "$MODEL_DIR/$RUN_BINARY" ]; then
  echo "[kernel] model run binary not found: $MODEL_DIR/$RUN_BINARY" >&2
  exit 1
fi

mkdir -p "$ROOTFS_ROOT"
cp -a "$MODEL_DIR"/. "$ROOTFS_ROOT"/
echo "[kernel] installed model runtime from $MODEL_DIR into /root/"
