#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A/B analysis: gold (dry) vs processed render.

Writes:
  - results/<run_id>_summary.csv   per-second metrics
  - results/<run_id>_overview.csv  one-row aggregate
  - results/<run_id>_report.md     human-readable report

Covers metrics B (scene loudness gap), C (steady noise segment vs source) and
E (true peak). A and D stay with `diagnose_stereo_noise.py`; F (preset
distinguishability) is a unit test.

Example:
  python3 scripts/analyze_ab.py \\
    --gold reference.wav \\
    --proc processed.wav \\
    --run-id my-run \\
    --seg-split 15 --noise-window 19:29 \\
    --notes "Strong preset, denoise on"
"""

from __future__ import annotations

import argparse
import csv
import math
import struct
import subprocess
import sys
from array import array
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path


def db(x: float, floor: float = 1e-12) -> float:
    return 20.0 * math.log10(max(abs(x), floor))


def load_mono_stereo(path: Path, sr: int = 48000) -> tuple[list[float], list[float], int]:
    """Return (L, R, sample_rate) as float lists via ffmpeg f32le."""
    raw = Path("/tmp") / f"_ab_{path.stem}_{sr}.f32"
    cmd = [
        "ffmpeg", "-y", "-i", str(path),
        "-f", "f32le", "-acodec", "pcm_f32le",
        "-ac", "2", "-ar", str(sr),
        str(raw),
    ]
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    data = raw.read_bytes()
    n = len(data) // 4
    samples = list(struct.unpack(f"<{n}f", data))
    L = samples[0::2]
    R = samples[1::2]
    return L, R, sr


def true_peak_db(path: Path, sr: int = 192000) -> float:
    """True-peak estimate (metric E): peak of a 4x-oversampled render.

    Streams ffmpeg output so a 192 kHz pass does not materialise in memory.
    """
    cmd = [
        "ffmpeg", "-v", "quiet", "-i", str(path),
        "-f", "f32le", "-acodec", "pcm_f32le",
        "-ac", "2", "-ar", str(sr),
        "pipe:1",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    assert proc.stdout is not None
    peak = 0.0
    tail = b""
    while True:
        chunk = proc.stdout.read(1 << 20)
        if not chunk:
            break
        buf = tail + chunk
        usable = len(buf) - (len(buf) % 4)
        tail = buf[usable:]
        block = array("f")
        block.frombytes(buf[:usable])
        for x in block:
            a = abs(x)
            if a > peak:
                peak = a
    proc.stdout.close()
    proc.wait()
    return db(peak)


def rms_window_db(L: list[float], R: list[float], a: int, b: int) -> float:
    """Mono-equivalent RMS in dB over sample range [a, b)."""
    a = max(0, a)
    b = min(len(L), len(R), b)
    if b <= a:
        return float("nan")
    return db(math.sqrt(0.5 * (rms(L[a:b]) ** 2 + rms(R[a:b]) ** 2)))


def parse_window(spec: str) -> tuple[float, float]:
    start, _, end = spec.partition(":")
    return float(start), float(end)


def rms(xs: list[float]) -> float:
    if not xs:
        return 0.0
    return math.sqrt(sum(x * x for x in xs) / len(xs))


def peak(xs: list[float]) -> float:
    return max((abs(x) for x in xs), default=0.0)


def jump_count(xs: list[float], thr: float = 0.15) -> int:
    return sum(1 for i in range(1, len(xs)) if abs(xs[i] - xs[i - 1]) > thr)


def mode_of(rL: float, rR: float, thr: float) -> str:
    actL, actR = rL > thr, rR > thr
    if not actL and not actR:
        return "silence"
    if actL and not actR:
        return "left_only"
    if actR and not actL:
        return "right_only"
    return "stereo"


def correlate(a: list[float], b: list[float], step: int = 100) -> float:
    aa = a[::step]
    bb = b[::step]
    m = min(len(aa), len(bb))
    if m < 8:
        return 0.0
    aa, bb = aa[:m], bb[:m]
    ma = sum(aa) / m
    mb = sum(bb) / m
    num = sum((x - ma) * (y - mb) for x, y in zip(aa, bb))
    den = math.sqrt(sum((x - ma) ** 2 for x in aa) * sum((y - mb) ** 2 for y in bb)) + 1e-20
    return num / den


def analyze(
    gold: Path,
    proc: Path,
    run_id: str,
    notes: str,
    out_dir: Path,
    seg_split: float = 15.0,
    baseline_gap: float | None = None,
    noise_window: tuple[float, float] | None = None,
    tp_ceiling: float = -1.0,
    preset: str | None = None,
    noise_target: float = -5.0,
) -> Path:
    gL, gR, sr = load_mono_stereo(gold)
    pL, pR, _ = load_mono_stereo(proc)
    n = min(len(gL), len(pL), len(gR), len(pR))
    gL, gR, pL, pR = gL[:n], gR[:n], pL[:n], pR[:n]
    thr = 10 ** (-70.0 / 20.0)
    secs = n // sr

    rows: list[dict] = []
    for s in range(secs):
        a, b = s * sr, (s + 1) * sr
        grL, grR = rms(gL[a:b]), rms(gR[a:b])
        prL, prR = rms(pL[a:b]), rms(pR[a:b])
        g_mode = mode_of(grL, grR, thr)
        p_mode = mode_of(prL, prR, thr)
        g_mono = math.sqrt(0.5 * (grL * grL + grR * grR))
        p_mono = math.sqrt(0.5 * (prL * prL + prR * prR))
        g_bal = db(grL) - db(grR) if g_mode == "stereo" else ""
        p_bal = db(prL) - db(prR) if p_mode == "stereo" else ""
        rows.append(
            {
                "sec": s,
                "gold_mode": g_mode,
                "proc_mode": p_mode,
                "gold_rms_db": round(db(g_mono), 3),
                "proc_rms_db": round(db(p_mono), 3),
                "delta_rms_db": round(db(p_mono) - db(g_mono), 3),
                "gold_bal_db": g_bal if g_bal == "" else round(g_bal, 3),
                "proc_bal_db": p_bal if p_bal == "" else round(p_bal, 3),
                "gold_peak_db": round(db(max(peak(gL[a:b]), peak(gR[a:b]))), 3),
                "proc_peak_db": round(db(max(peak(pL[a:b]), peak(pR[a:b]))), 3),
                "gold_jumps": jump_count(gL[a:b]),
                "proc_jumps": jump_count(pL[a:b]),
            }
        )

    # aggregates
    active = [r for r in rows if r["gold_mode"] != "silence" or r["proc_mode"] != "silence"]
    soft = [r for r in active if r["gold_rms_db"] < -28]
    loud = [r for r in active if r["gold_rms_db"] > -18]
    stereo_g = [r for r in rows if r["gold_mode"] == "stereo" and r["proc_mode"] == "stereo"
                and r["gold_bal_db"] != "" and r["proc_bal_db"] != ""]

    def mean(xs: list[float]) -> float:
        return sum(xs) / len(xs) if xs else float("nan")

    def median(xs: list[float]) -> float:
        if not xs:
            return float("nan")
        xs = sorted(xs)
        return xs[len(xs) // 2]

    g_modes = Counter(r["gold_mode"] for r in rows)
    p_modes = Counter(r["proc_mode"] for r in rows)

    mono_g = [0.5 * (gL[i] + gR[i]) for i in range(n)]
    mono_p = [0.5 * (pL[i] + pR[i]) for i in range(n)]
    corr = correlate(mono_g, mono_p)
    diff_e = sum((mono_p[i] - mono_g[i]) ** 2 for i in range(0, n, 8))
    en_e = sum(mono_g[i] ** 2 for i in range(0, n, 8)) + 1e-20
    rel_diff = diff_e / en_e

    # --- metric C: steady noise-segment level vs source ---
    noise_delta = float("nan")
    noise_gold_db = float("nan")
    noise_proc_db = float("nan")
    if noise_window is not None:
        a = int(noise_window[0] * sr)
        b = int(noise_window[1] * sr)
        noise_gold_db = rms_window_db(gL, gR, a, b)
        noise_proc_db = rms_window_db(pL, pR, a, b)
        noise_delta = noise_proc_db - noise_gold_db

    # --- metric B: scene loudness gap across the segment split,
    #     on dialogue-active seconds only (the declared noise window is
    #     excluded — the leveler lifting steady noise is not what B measures) ---
    def in_noise_window(s: float) -> bool:
        return noise_window is not None and noise_window[0] <= s < noise_window[1]

    seg_a = [r for r in active if r["sec"] < seg_split and not in_noise_window(r["sec"])]
    seg_b = [r for r in active if r["sec"] >= seg_split and not in_noise_window(r["sec"])]

    def seg_gap(key: str) -> float:
        if not seg_a or not seg_b:
            return float("nan")
        return abs(mean([r[key] for r in seg_a]) - mean([r[key] for r in seg_b]))

    gold_gap = seg_gap("gold_rms_db")
    proc_gap = seg_gap("proc_rms_db")
    gap_ref = baseline_gap if baseline_gap is not None else gold_gap
    gap_reduction = (
        100.0 * (gap_ref - proc_gap) / gap_ref
        if gap_ref and not math.isnan(gap_ref) and not math.isnan(proc_gap)
        else float("nan")
    )

    # --- metric C: steady noise-segment level vs source ---
    noise_delta = float("nan")
    noise_gold_db = float("nan")
    noise_proc_db = float("nan")
    if noise_window is not None:
        a = int(noise_window[0] * sr)
        b = int(noise_window[1] * sr)
        noise_gold_db = rms_window_db(gL, gR, a, b)
        noise_proc_db = rms_window_db(pL, pR, a, b)
        noise_delta = noise_proc_db - noise_gold_db

    # --- metric E: true peak (4x oversampled) ---
    gold_tp = true_peak_db(gold)
    proc_tp = true_peak_db(proc)

    def rnd(x: float) -> float | str:
        return "" if isinstance(x, float) and math.isnan(x) else round(x, 3)

    overview = {
        "run_id": run_id,
        "timestamp_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "gold": str(gold),
        "proc": str(proc),
        "duration_s": round(n / sr, 3),
        "sample_rate": sr,
        "notes": notes,
        "correlation": round(corr, 6),
        "diff_energy_ratio": f"{rel_diff:.6e}",
        "gold_left_only_s": g_modes.get("left_only", 0),
        "proc_left_only_s": p_modes.get("left_only", 0),
        "gold_stereo_s": g_modes.get("stereo", 0),
        "proc_stereo_s": p_modes.get("stereo", 0),
        "mean_delta_rms_db_active": round(mean([r["delta_rms_db"] for r in active]), 3),
        "mean_delta_rms_db_soft": round(mean([r["delta_rms_db"] for r in soft]), 3),
        "mean_delta_rms_db_loud": round(mean([r["delta_rms_db"] for r in loud]), 3),
        "median_abs_bal_gold_db": round(median([abs(float(r["gold_bal_db"])) for r in stereo_g]), 3)
        if stereo_g else "",
        "median_abs_bal_proc_db": round(median([abs(float(r["proc_bal_db"])) for r in stereo_g]), 3)
        if stereo_g else "",
        "gold_jumps_total": sum(r["gold_jumps"] for r in rows),
        "proc_jumps_total": sum(r["proc_jumps"] for r in rows),
        "seg_split_s": seg_split,
        "gold_seg_gap_db": rnd(gold_gap),
        "proc_seg_gap_db": rnd(proc_gap),
        "seg_gap_baseline_db": rnd(gap_ref),
        "seg_gap_reduction_pct": rnd(gap_reduction),
        "noise_window_s": f"{noise_window[0]}:{noise_window[1]}" if noise_window else "",
        "noise_gold_rms_db": rnd(noise_gold_db),
        "noise_proc_rms_db": rnd(noise_proc_db),
        "noise_delta_db": rnd(noise_delta),
        "gold_true_peak_dbtp": rnd(gold_tp),
        "proc_true_peak_dbtp": rnd(proc_tp),
        "tp_ceiling_dbtp": tp_ceiling,
        "pass_channel_repair": p_modes.get("left_only", 0) <= max(1, g_modes.get("left_only", 0) // 5),
        "pass_soft_boost": (mean([r["delta_rms_db"] for r in soft]) > 2.0) if soft else False,
        "pass_no_crackle": sum(r["proc_jumps"] for r in rows) < sum(r["gold_jumps"] for r in rows) + 50
        or sum(r["proc_jumps"] for r in rows) < secs * 5,
        # metric B — gap must drop >= 40% vs baseline (test7) or vs gold when no baseline
        "pass_seg_gap": (not math.isnan(gap_reduction)) and gap_reduction >= 40.0,
        # metric C — noise segment >= 4 dB below source
        # Metric C is judged for the Clean preset only; Soft/Strong
        # report the value without a pass line (their leveler lifts noise by
        # design). Target -5 dB: unit-level reduction is -4.4 dB, so -4 leaves
        # only 0.4 dB of room for real-material drift.
        "pass_noise_floor": (preset == "clean") and (not math.isnan(noise_delta)) and noise_delta <= noise_target,
        # metric E — true peak under ceiling (0.1 dB resampler tolerance)
        "pass_true_peak": proc_tp <= tp_ceiling + 0.1,
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    summary_csv = out_dir / f"{run_id}_summary.csv"
    overview_csv = out_dir / f"{run_id}_overview.csv"
    report_md = out_dir / f"{run_id}_report.md"

    with summary_csv.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["sec"])
        w.writeheader()
        w.writerows(rows)

    with overview_csv.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(overview.keys()))
        w.writeheader()
        w.writerow(overview)

    # MD report
    lines = [
        f"# A/B Report — `{run_id}`",
        "",
        f"- **UTC**: {overview['timestamp_utc']}",
        f"- **Gold**: `{gold}`",
        f"- **Processed**: `{proc}`",
        f"- **Duration compared**: {overview['duration_s']} s @ {sr} Hz",
        f"- **Notes**: {notes or '_(none)_'}",
        "",
        "## Pass / fail (heuristic)",
        "",
        f"| Check | Result |",
        f"|---|---|",
        f"| Channel repair (L-only reduced) | {'PASS' if overview['pass_channel_repair'] else 'FAIL'} "
        f"({overview['gold_left_only_s']}s → {overview['proc_left_only_s']}s) |",
        f"| Soft boost (soft secs mean ΔRMS > +2 dB) | {'PASS' if overview['pass_soft_boost'] else 'FAIL'} "
        f"(mean Δ = {overview['mean_delta_rms_db_soft']} dB) |",
        f"| No crackle (jump count) | {'PASS' if overview['pass_no_crackle'] else 'FAIL'} "
        f"(gold {overview['gold_jumps_total']} → proc {overview['proc_jumps_total']}) |",
        f"| B — scene gap −40% vs baseline | {'PASS' if overview['pass_seg_gap'] else 'FAIL'} "
        f"(ref {overview['seg_gap_baseline_db']} → proc {overview['proc_seg_gap_db']} dB, "
        f"{overview['seg_gap_reduction_pct']}%) |",
        f"| C — noise segment ≤ {noise_target} dB vs source (Clean only) | "
        f"{'PASS' if overview['pass_noise_floor'] else ('FAIL' if noise_window and preset == 'clean' else 'n/a')} "
        f"(Δ = {overview['noise_delta_db']} dB, window {overview['noise_window_s'] or '—'}) |",
        f"| E — true peak ≤ {tp_ceiling} dBTP | {'PASS' if overview['pass_true_peak'] else 'FAIL'} "
        f"(gold {overview['gold_true_peak_dbtp']} → proc {overview['proc_true_peak_dbtp']} dBTP) |",
        "",
        "## Aggregate",
        "",
        f"| Metric | Value |",
        f"|---|---|",
        f"| Correlation | {overview['correlation']} |",
        f"| Diff energy / gold | {overview['diff_energy_ratio']} |",
        f"| Mean ΔRMS (active) | {overview['mean_delta_rms_db_active']} dB |",
        f"| Mean ΔRMS (soft < −28 dB) | {overview['mean_delta_rms_db_soft']} dB |",
        f"| Mean ΔRMS (loud > −18 dB) | {overview['mean_delta_rms_db_loud']} dB |",
        f"| Scene gap gold / proc (split {seg_split}s) | "
        f"{overview['gold_seg_gap_db']} / {overview['proc_seg_gap_db']} dB |",
        f"| Noise window gold / proc | "
        f"{overview['noise_gold_rms_db']} / {overview['noise_proc_rms_db']} dB |",
        f"| True peak gold / proc | "
        f"{overview['gold_true_peak_dbtp']} / {overview['proc_true_peak_dbtp']} dBTP |",
        f"| Median |bal| gold / proc | {overview['median_abs_bal_gold_db']} / {overview['median_abs_bal_proc_db']} dB |",
        f"| Mode gold | {dict(g_modes)} |",
        f"| Mode proc | {dict(p_modes)} |",
        "",
        "## Per-second (excerpt: non-silence)",
        "",
        "| sec | gold mode | proc mode | gold RMS | proc RMS | ΔRMS | gold bal | proc bal | jumps g→p |",
        "|---:|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        if r["gold_mode"] == "silence" and r["proc_mode"] == "silence":
            continue
        lines.append(
            f"| {r['sec']} | {r['gold_mode']} | {r['proc_mode']} | {r['gold_rms_db']} | {r['proc_rms_db']} | "
            f"{r['delta_rms_db']:+} | {r['gold_bal_db']} | {r['proc_bal_db']} | "
            f"{r['gold_jumps']}→{r['proc_jumps']} |"
        )
    lines += [
        "",
        "## Files",
        "",
        f"- `{summary_csv.name}` — per-second CSV",
        f"- `{overview_csv.name}` — one-row overview CSV",
        f"- `{report_md.name}` — this report",
        "",
    ]
    report_md.write_text("\n".join(lines) + "\n", encoding="utf-8")

    # append index
    index = out_dir / "INDEX.md"
    entry = (
        f"| {overview['timestamp_utc']} | `{run_id}` | {preset or '-'} | "
        f"{'Y' if overview['pass_channel_repair'] else 'N'}/"
        f"{'Y' if overview['pass_soft_boost'] else 'N'}/"
        f"{'Y' if overview['pass_no_crackle'] else 'N'} | "
        f"{'Y' if overview['pass_seg_gap'] else 'N'}/"
        f"{'Y' if overview['pass_noise_floor'] else ('N' if noise_window else '-')}/"
        f"{'Y' if overview['pass_true_peak'] else 'N'} | "
        f"{overview['mean_delta_rms_db_soft']} | {overview['proc_seg_gap_db']} | "
        f"{overview['noise_delta_db']} | {overview['proc_true_peak_dbtp']} | "
        f"{overview['correlation']} | {notes.replace('|', '/')} |\n"
    )
    if not index.exists():
        index.write_text(
            "# Results index\n\n"
            "| UTC | run_id | preset | pass CR/soft/crackle | pass B/C/E | soft ΔRMS dB | "
            "seg gap dB | noise Δ dB | TP dBTP | corr | notes |\n"
            "|---|---|---|---|---|---:|---:|---:|---:|---:|---|\n",
            encoding="utf-8",
        )
    with index.open("a", encoding="utf-8") as f:
        f.write(entry)

    print(f"Wrote {summary_csv}")
    print(f"Wrote {overview_csv}")
    print(f"Wrote {report_md}")
    print(f"Updated {index}")
    return report_md


def main() -> int:
    ap = argparse.ArgumentParser(description="Gold vs processed A/B metrics → CSV + MD")
    ap.add_argument("--gold", required=True, type=Path)
    ap.add_argument("--proc", required=True, type=Path)
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--notes", default="")
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "testdata" / "results",
    )
    ap.add_argument(
        "--seg-split",
        type=float,
        default=15.0,
        help="Second boundary for the scene-gap metric B (default 15 → 0–14 vs 15+)",
    )
    ap.add_argument(
        "--baseline-gap",
        type=float,
        default=None,
        help="Reference scene gap in dB (e.g. test7) for metric B; defaults to the gold gap",
    )
    ap.add_argument(
        "--noise-window",
        default=None,
        help="Steady noise window in relative seconds START:END for metric C (e.g. 19:29)",
    )
    ap.add_argument(
        "--tp-ceiling",
        type=float,
        default=-1.0,
        help="True-peak ceiling in dBTP for metric E (default -1.0)",
    )
    ap.add_argument(
        "--preset",
        choices=("soft", "strong", "clean"),
        default=None,
        help="Preset of the processed render. Metric C is only judged for clean; "
        "other presets report the noise-window value without a pass line.",
    )
    ap.add_argument(
        "--noise-target",
        type=float,
        default=-5.0,
        help="Metric C target in dB vs source (default -5; -4 leaves too little "
        "room for real-material drift)",
    )
    args = ap.parse_args()
    if not args.gold.exists():
        print(f"missing gold: {args.gold}", file=sys.stderr)
        return 1
    if not args.proc.exists():
        print(f"missing proc: {args.proc}", file=sys.stderr)
        return 1
    analyze(
        args.gold,
        args.proc,
        args.run_id,
        args.notes,
        args.out_dir,
        seg_split=args.seg_split,
        baseline_gap=args.baseline_gap,
        noise_window=parse_window(args.noise_window) if args.noise_window else None,
        tp_ceiling=args.tp_ceiling,
        preset=args.preset,
        noise_target=args.noise_target,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
