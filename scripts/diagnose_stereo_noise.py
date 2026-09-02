#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Red-capable loop: electrical-noise proxy on stereo vs left_only seconds.

Exit 1 if stereo HF excess vs gold exceeds threshold while left_only stays clean.
"""
from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


def load(path: Path, sr: int = 48000):
    raw = Path("/tmp") / f"_diag_{path.stem}.f32"
    subprocess.check_call(
        ["ffmpeg", "-y", "-i", str(path), "-f", "f32le", "-ac", "2", "-ar", str(sr), str(raw)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    data = raw.read_bytes()
    n = len(data) // 4
    s = list(struct.unpack(f"<{n}f", data))
    return s[0::2], s[1::2], sr


def rms(xs):
    return math.sqrt(sum(v * v for v in xs) / max(1, len(xs)))


def hf_proxy(xs):
    d = sum((xs[i] - xs[i - 1]) ** 2 for i in range(1, len(xs)))
    e = sum(v * v for v in xs) + 1e-20
    return d / e


def mode_sec(L, R, thr=10 ** (-60 / 20)):
    rL, rR = rms(L), rms(R)
    if rL <= thr and rR <= thr:
        return "silence"
    if rL > thr and rR <= thr:
        return "left_only"
    if rR > thr and rL <= thr:
        return "right_only"
    return "stereo"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gold", type=Path, required=True)
    ap.add_argument("--proc", type=Path, required=True)
    ap.add_argument("--max-stereo-hf-excess", type=float, default=0.04)
    ap.add_argument("--max-lonly-hf-excess", type=float, default=0.02)
    args = ap.parse_args()

    gL, gR, sr = load(args.gold)
    pL, pR, _ = load(args.proc)
    n = min(len(gL), len(pL))
    gL, gR, pL, pR = gL[:n], gR[:n], pL[:n], pR[:n]

    acc = defaultdict(lambda: {"g": [], "p": []})
    for s in range(n // sr):
        a, b = s * sr, (s + 1) * sr
        m = mode_sec(gL[a:b], gR[a:b])
        gm = [0.5 * (gL[i] + gR[i]) for i in range(a, b)]
        pm = [0.5 * (pL[i] + pR[i]) for i in range(a, b)]
        acc[m]["g"].append(hf_proxy(gm))
        acc[m]["p"].append(hf_proxy(pm))

    def mean(xs):
        return sum(xs) / len(xs) if xs else 0.0

    stereo_ex = mean(acc["stereo"]["p"]) - mean(acc["stereo"]["g"])
    lonly_ex = mean(acc["left_only"]["p"]) - mean(acc["left_only"]["g"])

    print(f"stereo HF: gold={mean(acc['stereo']['g']):.4f} proc={mean(acc['stereo']['p']):.4f} excess={stereo_ex:+.4f}")
    print(f"left_only HF: gold={mean(acc['left_only']['g']):.4f} proc={mean(acc['left_only']['p']):.4f} excess={lonly_ex:+.4f}")

    bad = False
    if stereo_ex > args.max_stereo_hf_excess:
        print(f"FAIL stereo HF excess {stereo_ex:.4f} > {args.max_stereo_hf_excess}")
        bad = True
    else:
        print(f"PASS stereo HF excess {stereo_ex:.4f}")

    if lonly_ex > args.max_lonly_hf_excess:
        print(f"FAIL left_only HF excess {lonly_ex:.4f} > {args.max_lonly_hf_excess}")
        bad = True
    else:
        print(f"PASS left_only HF excess {lonly_ex:.4f}")

    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
