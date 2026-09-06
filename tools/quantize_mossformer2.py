#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Quantize the exported MossFormer2 ONNX to INT8 (dynamic) and, when given
two renders (--fp32-wav / --int8-wav), report the FP32 vs INT8 A/B metrics:
p10/p25/p95 dynamics (50 ms frames, 25 ms hop) plus a coarse spectral tilt
comparison — the plan's INT8 quality gate.

Usage:
  python3 tools/quantize_mossformer2.py --fp32 in.onnx --out int8.onnx
  python3 tools/quantize_mossformer2.py --fp32 in.onnx --out int8.onnx \
      --fp32-wav a.wav --int8-wav b.wav
"""
from __future__ import annotations

import argparse
import math
import struct
import subprocess
from pathlib import Path


def load_mono(path: Path, sr: int = 48000) -> list[float]:
    raw = Path("/tmp") / f"_q_{path.stem}_{sr}.f32"
    subprocess.check_call([
        "ffmpeg", "-y", "-i", str(path), "-f", "f32le", "-acodec", "pcm_f32le",
        "-ac", "2", "-ar", str(sr), str(raw),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    data = raw.read_bytes()
    n = len(data) // 4
    xs = struct.unpack(f"<{n}f", data)
    return [0.5 * (xs[i] + xs[i + 1]) for i in range(0, n - 1, 2)]


def frame_rms_db(xs: list[float], sr: int, trim_end: float) -> list[float]:
    frame, hop = int(0.05 * sr), int(0.025 * sr)
    out = []
    pos = 0
    end = len(xs) - int(trim_end * sr)
    while pos + frame <= end:
        acc = sum(x * x for x in xs[pos:pos + frame])
        out.append(10 * math.log10(max(acc / frame, 1e-12)))
        pos += hop
    return out


def percentile(xs: list[float], p: float) -> float:
    s = sorted(xs)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    return s[lo] * (1 - (k - lo)) + s[min(lo + 1, len(s) - 1)] * (k - lo)


def band_energy(xs: list[float], sr: int, lo: float, hi: float) -> float:
    """Rough band energy via ffmpeg aformat/lowpass+highpass, log scale."""
    raw = Path("/tmp") / f"_q_band_{lo}_{hi}.f32"
    subprocess.check_call([
        "ffmpeg", "-y", "-i", "pipe:0", "-af",
        f"highpass=f={lo},lowpass=f={hi}", "-f", "f32le", "-ac", "1",
        "-acodec", "pcm_f32le", "-ar", str(sr), str(raw),
    ], input=struct.pack(f"<{len(xs)}f", *xs), stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    data = raw.read_bytes()
    n = len(data) // 4
    xs2 = struct.unpack(f"<{n}f", data)
    acc = sum(x * x for x in xs2)
    return 10 * math.log10(max(acc / max(n, 1), 1e-12))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fp32", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--fp32-wav", type=Path, default=None)
    ap.add_argument("--int8-wav", type=Path, default=None)
    ap.add_argument("--trim-end", type=float, default=0.0)
    ap.add_argument("--calibrate-wav", type=Path, default=None,
                    help="wav for static-QDQ calibration; omit to fall back "
                         "to dynamic quantization")
    args = ap.parse_args()

    from onnxruntime.quantization import (CalibrationDataReader, quantize_static,
                                          QuantFormat, QuantType, QuantizationMode)

    # Static QDQ quantization: dynamic quantization emits ConvInteger, which
    # ORT 1.22 on Apple Silicon has no kernel for; QDQ (QLinearConv) is the
    # portable path and quality-checked by the gate below anyway.
    import numpy as np
    import onnxruntime as ort

    class WavCalibrationReader (CalibrationDataReader):
        def __init__ (self, wav: Path, window: int = 192000, max_windows: int = 8):
            import soundfile as sf
            data, sr = sf.read(str(wav), dtype="float32", always_2d=True)
            mono = data.T.mean(axis=0)
            self.chunks = [mono[i:i + window].astype(np.float32)
                           for i in range(0, len(mono) - window, window)][:max_windows]
            self.idx = 0
        def get_next (self):
            if self.idx >= len(self.chunks):
                return None
            c = self.chunks[self.idx]
            self.idx += 1
            return {"input": c.reshape(1, -1)}

    calib_wav = args.calibrate_wav
    if calib_wav is None or not calib_wav.exists():
        print("no --calibrate-wav; falling back to dynamic quantization")
        from onnxruntime.quantization import quantize_dynamic, QuantType
        quantize_dynamic(str(args.fp32), str(args.out), weight_type=QuantType.QInt8)
    else:
        reader = WavCalibrationReader(calib_wav)
        quantize_static(str(args.fp32), str(args.out),
                        calibration_data_reader=reader,
                        quant_format=QuantFormat.QDQ,
                        activation_type=QuantType.QInt8,
                        weight_type=QuantType.QInt8,
                        per_channel=True)

    fp32_mb = args.fp32.stat().st_size / 1e6
    int8_mb = args.out.stat().st_size / 1e6
    print(f"quantized: {fp32_mb:.1f} MB -> {int8_mb:.1f} MB")

    if args.fp32_wav and args.int8_wav:
        sr = 48000
        a = load_mono(args.fp32_wav, sr)
        b = load_mono(args.int8_wav, sr)
        n = min(len(a), len(b))
        a, b = a[:n], b[:n]
        fa = frame_rms_db(a, sr, args.trim_end)
        fb = frame_rms_db(b, sr, args.trim_end)
        for name, p in (("p10", 10), ("p25", 25), ("p95", 95)):
            print(f"{name}: fp32 {percentile(fa, p):.2f} dB, int8 {percentile(fb, p):.2f} dB, "
                  f"diff {percentile(fb, p) - percentile(fa, p):+.3f} dB")
        print(f"dyn p95-p25: fp32 {percentile(fa, 95) - percentile(fa, 25):.2f}, "
              f"int8 {percentile(fb, 95) - percentile(fb, 25):.2f}")
        for lo, hi in ((50, 500), (500, 4000), (4000, 12000), (12000, 22000)):
            ea = band_energy(a, sr, lo, hi)
            eb = band_energy(b, sr, lo, hi)
            print(f"band {lo}-{hi} Hz: fp32 {ea:.2f} dB, int8 {eb:.2f} dB, diff {eb - ea:+.3f} dB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
