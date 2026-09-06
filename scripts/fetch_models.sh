#!/usr/bin/env bash
# Fetch the MossFormer2 SE 48K checkpoint and the ONNX Runtime C++ release
# used by the HQ tier, plus quantize the exported ONNX model.
# SPDX-License-Identifier: AGPL-3.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/third_party/ort" "$ROOT/third_party/mossformer2"

# --- ONNX Runtime (universal2) ---
ORT_VERSION="${ORT_VERSION:-1.22.0}"
if [ ! -d "$ROOT/third_party/ort/onnxruntime-osx-universal2-$ORT_VERSION" ]; then
    curl -sL -o /tmp/ort.tgz \
        "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/onnxruntime-osx-universal2-$ORT_VERSION.tgz"
    tar xzf /tmp/ort.tgz -C "$ROOT/third_party/ort"
fi

# --- MossFormer2 weights (Apache-2.0, alibabasglab/MossFormer2_SE_48K) ---
CKPT="$ROOT/third_party/mossformer2/last_best_checkpoint.pt"
if [ ! -f "$CKPT" ]; then
    curl -sL -o "$CKPT" \
        "https://huggingface.co/alibabasglab/MossFormer2_SE_48K/resolve/main/last_best_checkpoint.pt"
fi

# --- Export FP32 ONNX (needs python3.13 venv with torch; see plan Step 1b) ---
ONNX="$ROOT/third_party/mossformer2/mossformer2_fp32.onnx"
if [ ! -f "$ONNX" ]; then
    python3 scripts/export_mossformer2_onnx.py \
        --checkpoint "$CKPT" --output "$ONNX" --window 192000
fi

# NOTE: the HQ tier ships the FP32 model (~278 MB). INT8 (dynamic and QDQ
# static) was quantized and REJECTED by the quality gate: same-window
# correlation vs FP32 was 0.83 (dynamic) / 0.66 (QDQ static) — see pitfall
# 2026-09-06. tools/quantize_mossformer2.py remains for future experiments.

echo "third_party ready."
