#!/usr/bin/env bash
# Build the libDF static library (universal arm64 + x86_64) for SyncTrack Prep.
# Requires: rustup (with both apple targets), git, Xcode CLT.
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail

SRC_DIR="${1:-$HOME/nn-deps/df}"
REPO_URL="https://github.com/Rikorose/DeepFilterNet.git"

if [ ! -d "$SRC_DIR" ]; then
    git clone --depth 1 "$REPO_URL" "$SRC_DIR"
fi

rustup target add aarch64-apple-darwin x86_64-apple-darwin

cd "$SRC_DIR"
cargo build -p deep_filter --release --target aarch64-apple-darwin
cargo build -p deep_filter --release --target x86_64-apple-darwin

OUT_DIR="$(cd "$(dirname "$0")/.." && pwd)/third_party/dfn/lib"
mkdir -p "$OUT_DIR"
lipo -create \
    target/aarch64-apple-darwin/release/libdf.a \
    target/x86_64-apple-darwin/release/libdf.a \
    -output "$OUT_DIR/libdf_universal.a"
lipo -info "$OUT_DIR/libdf_universal.a"
echo "Wrote $OUT_DIR/libdf_universal.a"

MODEL_DIR="$(cd "$(dirname "$0")/.." && pwd)/third_party/dfn/model"
mkdir -p "$MODEL_DIR"
cp models/DeepFilterNet3_onnx.tar.gz "$MODEL_DIR/"
echo "Wrote $MODEL_DIR/DeepFilterNet3_onnx.tar.gz"
