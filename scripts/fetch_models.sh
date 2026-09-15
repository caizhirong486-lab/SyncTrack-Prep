#!/usr/bin/env bash
# Fetch the MossFormer2 SE 48K checkpoint and the ONNX Runtime C++ release
# used by the HQ tier, then reproduce both the dynamic shipping graph and the
# retired fixed graph used only by the DOP equivalence gate.
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
ORT_DYLIB="$ROOT/third_party/ort/onnxruntime-osx-universal2-$ORT_VERSION/lib/libonnxruntime.dylib"
if [ -f "$ORT_DYLIB" ] && command -v install_name_tool >/dev/null 2>&1; then
    install_name_tool -id @rpath/libonnxruntime.dylib "$ORT_DYLIB"
fi

# --- MossFormer2 weights (Apache-2.0, alibabasglab/MossFormer2_SE_48K) ---
CKPT="$ROOT/third_party/mossformer2/last_best_checkpoint.pt"
if [ ! -f "$CKPT" ]; then
    curl -sL -o "$CKPT" \
        "https://huggingface.co/alibabasglab/MossFormer2_SE_48K/resolve/main/last_best_checkpoint.pt"
fi

# --- Export FP32 ONNX (set STP_PYTHON_BIN to the ClearerVoice venv) ---
STP_PYTHON_BIN="${STP_PYTHON_BIN:-python3}"
ONNX="$ROOT/third_party/mossformer2/mossformer2_dynamic.onnx"
LEGACY_ONNX="$ROOT/third_party/mossformer2/mossformer2_fp32.onnx"
GOLDEN="$ROOT/tests/golden/mossformer/golden.sha256"
if [ ! -f "$ONNX" ] || [ ! -f "$ROOT/third_party/mossformer2/mel60_2048.f32" ] \
   || [ ! -f "$ROOT/third_party/mossformer2/dop_dither.f32" ] || [ ! -f "$GOLDEN" ]; then
    (cd "$ROOT" && "$STP_PYTHON_BIN" scripts/export_mossformer2_dynamic.py \
        --checkpoint "$CKPT" --output "$ONNX")
fi
if [ ! -f "$LEGACY_ONNX" ]; then
    (cd "$ROOT" && "$STP_PYTHON_BIN" scripts/export_mossformer2_onnx.py \
        --checkpoint "$CKPT" --output "$LEGACY_ONNX" --window 192000)
fi

# NOTE: the HQ tier ships the FP32 model (~278 MB). INT8 (dynamic and QDQ
# static) was quantized and REJECTED by the quality gate: same-window
# correlation vs FP32 was 0.83 (dynamic) / 0.66 (QDQ static) — see pitfall
# 2026-09-06. tools/quantize_mossformer2.py remains for future experiments.

echo "third_party ready."
