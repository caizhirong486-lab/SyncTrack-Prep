#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Leveler noise-gate threshold diagnosis.

The gate discriminates steady noise from dialogue by the fast envelope's
deviation from its own short-time mean (modAvg). The enter/exit thresholds
must sit between the two distributions: enter below the noise window's p10,
exit above the dialogue window's p90 — otherwise the gate flutters in the
overlap.

This is a faithful Python replica of Leveler's envelope/modulation estimate
(src/dsp/Leveler.cpp), driven from a WAV. Validate the replica against the
C++ on synthetic signals first, then point it at the real clip.

Usage:
  python3 scripts/diagnose_gate.py /tmp/gold.wav --noise-window 19:29
  python3 scripts/diagnose_gate.py /tmp/synth.wav --self-test   # replica check
"""

from __future__ import annotations

import argparse
import math
import struct
import subprocess
from pathlib import Path

SR = 48000.0
# Leveler constants (keep in sync with src/dsp/Leveler.h)
FAST_TAU = 0.04
FAST_AVG_TAU = 0.20
MOD_TAU = 0.12


def leveler_mod_series(samples: list[float]) -> list[float]:
    """Replica of Leveler::process envelope + modulation estimate."""
    fast_env = fast_avg = mod_avg = 0.0
    fast_coef = 1.0 - math.exp(-1.0 / (SR * max(0.01, FAST_TAU)))
    avg_coef = 1.0 - math.exp(-1.0 / (SR * FAST_AVG_TAU))
    mod_coef = 1.0 - math.exp(-1.0 / (SR * MOD_TAU))
    out: list[float] = []
    for x in samples:
        amp = abs(x)
        fast_env += fast_coef * (amp - fast_env)
        fast_avg += avg_coef * (amp - fast_avg)
        dev = abs(fast_env - fast_avg) / max(fast_avg, 1e-6)
        mod_avg += mod_coef * (dev - mod_avg)
        out.append(mod_avg)
    return out


def load_mono(path: Path) -> list[float]:
    raw = Path("/tmp") / f"_gate_{path.stem}.f32"
    cmd = ["ffmpeg", "-y", "-i", str(path), "-f", "f32le", "-acodec", "pcm_f32le",
           "-ac", "2", "-ar", "48000", str(raw)]
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    data = raw.read_bytes()
    n = len(data) // 4
    s = list(struct.unpack(f"<{n}f", data))
    return [0.5 * (s[i] + s[i + 1]) for i in range(0, len(s), 2)]


def quantile(xs: list[float], q: float) -> float:
    if not xs:
        return float("nan")
    xs = sorted(xs)
    k = int(q * (len(xs) - 1))
    return xs[k]


def report(name: str, xs: list[float]) -> None:
    xs = [x for x in xs if x > 0.0]
    print(f"  {name:16s} n={len(xs):6d}  p10={quantile(xs, 0.10):.4f}  "
          f"p50={quantile(xs, 0.50):.4f}  p90={quantile(xs, 0.90):.4f}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", type=Path)
    ap.add_argument("--noise-window", default=None, help="START:END seconds of the noise section")
    ap.add_argument("--dialogue-window", default=None, help="START:END seconds of quiet dialogue")
    ap.add_argument("--self-test", action="store_true", help="validate the replica on synthetic signals")
    args = ap.parse_args()

    if args.self_test:
        import random
        rng = random.Random(7)
        noise = [rng.gauss(0.0, 0.02) for _ in range(48000 * 2)]
        print("self-test: steady white noise modAvg should be ~0.03 (below enter thr 0.08)")
        report("white noise", leveler_mod_series(noise))
        vowel = [0.1 * math.sin(2 * math.pi * 220 * i / SR) for i in range(48000 * 2)]
        print("self-test: sustained vowel modAvg should stay low but NOT cross the noise line")
        report("vowel", leveler_mod_series(vowel))
        whisper = [0.03 * rng.gauss(0.0, 1.0) * (0.6 + 0.4 * math.sin(2 * math.pi * 4 * i / SR))
                   for i in range(48000 * 2)]
        print("self-test: whisper (modulated) should sit above enter thr")
        report("whisper", leveler_mod_series(whisper))
        return 0

    if not args.wav.exists():
        print(f"missing: {args.wav}", file=sys.stderr)
        return 1

    mono = load_mono(args.wav)
    mod = leveler_mod_series(mono)
    n_sec = len(mono) // int(SR)

    if args.noise_window:
        a, b = (int(x) for x in args.noise_window.split(":"))
        report(f"noise {a}-{b}s", mod[a * int(SR): b * int(SR)])
    if args.dialogue_window:
        a, b = (int(x) for x in args.dialogue_window.split(":"))
        report(f"dialogue {a}-{b}s", mod[a * int(SR): b * int(SR)])

    print("\nRule: enter threshold < noise p10; exit threshold > dialogue p90.")
    print("Current: enter 0.08 / exit 0.15.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
