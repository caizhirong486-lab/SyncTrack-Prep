#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Export the MossFormer2_SE_48K MaskNet as a DYNAMIC ONNX graph.

The dynamic graph takes the ClearerVoice fbank/delta features directly:

    feats [channels, frames, 180] -> mask [channels, frames, 961]

The C++ plugin implements the int16-domain frontend (Kaldi fbank, deltas),
the 1920-point STFT, masking and the iSTFT/OLA backend, so the exported
graph is MaskNet-only. The same graph serves the 160 ms short window
(frames=16) and the 4 s DOP window (frames=496).

Also emits, from the same environment (so parity is guaranteed):

  - resources: mel bank matrix (60x1025) and the DOP dither table
    (496x1920, torch seed 20260906 — the exact sequence burned into the
    retired fixed-graph export), plus small golden vectors for the C++
    frontend/backend unit tests.
  - report: shape/parity/size/CPU-benchmark summary.

Short-window dither uses a counter-based hash stream (splitmix64 +
Box-Muller) addressed by the generation-relative global fbank frame index;
the golden vectors pin that algorithm for the C++ implementation.

Usage:
  ~/nn-deps/cv/bin/python scripts/export_mossformer2_dynamic.py \
      --checkpoint third_party/mossformer2/last_best_checkpoint.pt \
      --output third_party/mossformer2/mossformer2_dynamic.onnx \
      [--golden-dir tests/golden/mossformer] [--skip-export]
"""
from __future__ import annotations

import argparse
import hashlib
import math
import struct
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import torch

CV_STUDIO = Path.home() / "nn-deps" / "cv-studio"
if not CV_STUDIO.exists():
    CV_STUDIO = Path(__file__).resolve().parents[2] / "cv-studio"
sys.path.insert(0, str(CV_STUDIO / "clearvoice"))
sys.path.insert(0, str(CV_STUDIO))

from clearvoice.models.mossformer2_se.mossformer2_se_wrapper import MossFormer2_SE_48K  # noqa: E402

MAX_WAV_VALUE = 32768.0
DITHER_SEED = 20260906  # matches the retired fixed-graph export

# Short-window streaming geometry (48 kHz engine domain).
SHORT_WINDOW = 7680    # 160 ms
SHORT_STRIDE = 5760    # 120 ms = 15 fbank hops
SHORT_TRIM = 960       # 20 ms per edge
SHORT_FRAMES = (SHORT_WINDOW - 1920) // 384 + 1   # 16
DOP_WINDOW = 192000    # 4 s
DOP_FRAMES = (DOP_WINDOW - 1920) // 384 + 1       # 496

ARGS = SimpleNamespace(
    win_type="hamming", win_len=1920, win_inc=384, fft_len=1920,
    num_mels=60, sampling_rate=48000,
)


# ---------------------------------------------------------------------------
# Short-window dither stream: splitmix64 counter hash + Box-Muller. Mirrored
# in C++ (MossFormerFrontend.cpp); both sides must produce the same values to
# within 1e-9 (double math, no platform-sensitive bit pinning required).
# ---------------------------------------------------------------------------
def splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return x ^ (x >> 31)


def short_dither_frame(frame_index: int) -> np.ndarray:
    """One frame (1920 float32 samples) of deterministic dither noise."""
    frame_index &= 0xFFFFFFFFFFFFFFFF
    out = np.empty(1920, dtype=np.float32)
    state = splitmix64(DITHER_SEED ^ (frame_index * 0x9E3779B97F4A7C15 & 0xFFFFFFFFFFFFFFFF))
    state &= 0xFFFFFFFFFFFFFFFF
    i = 0
    while i < 1920:
        state = splitmix64(state)
        u1 = ((state >> 11) * (2.0 ** -53)) or 2.0 ** -53
        state = splitmix64(state)
        u2 = (state >> 11) * (2.0 ** -53)
        r = math.sqrt(-2.0 * math.log(u1))
        out[i] = r * math.cos(2.0 * math.pi * u2)
        if i + 1 < 1920:
            out[i + 1] = r * math.sin(2.0 * math.pi * u2)
        i += 2
    return out


def write_f32(path: Path, arr: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    np.ascontiguousarray(arr, dtype=np.float32).tofile(path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--golden-dir", type=Path, default=Path("tests/golden/mossformer"))
    ap.add_argument("--resource-dir", type=Path, default=Path("third_party/mossformer2"))
    ap.add_argument("--skip-export", action="store_true",
                    help="only (re)generate resources + golden vectors")
    args_cli = ap.parse_args()

    print("Loading ClearerVoice MossFormer2_SE_48K ...")
    wrapper = MossFormer2_SE_48K(ARGS)
    state = torch.load(args_cli.checkpoint, map_location="cpu", weights_only=True)
    wrapper.model.load_state_dict(state)
    wrapper.model.eval()
    masknet = wrapper.model  # TestNet: [B, S, 180] -> list, [-1] is [B, S, 961]

    # ------------------------------------------------------------------ export
    if not args_cli.skip_export:
        probe = torch.randn(2, SHORT_FRAMES, 180)
        out_path = args_cli.output
        out_path.parent.mkdir(parents=True, exist_ok=True)
        print(f"Exporting dynamic MaskNet (probe {tuple(probe.shape)}) ...")
        torch.onnx.export(
            masknet, probe, str(out_path),
            input_names=["feats"], output_names=["mask"],
            dynamic_axes={"feats": {0: "channels", 1: "frames"},
                          "mask": {0: "channels", 1: "frames"}},
            opset_version=18, do_constant_folding=True,
            dynamo=False,  # dynamo exporter breaks on this model (pitfall 2026-09-06)
        )
        size_mb = out_path.stat().st_size / 1e6
        print(f"Wrote {out_path} ({size_mb:.1f} MB)")

        # shape + parity gate: batch 1/2 x frames 16/496
        import onnx
        import onnxruntime as ort
        exported = onnx.load(str(out_path))
        opsets = {i.domain: i.version for i in exported.opset_import}
        assert opsets.get("") == 18, f"opset mismatch: {opsets}"
        ext = sum(i.data_location == onnx.TensorProto.EXTERNAL
                  for i in exported.graph.initializer)
        assert ext == 0, f"{ext} external initializers; bundle needs one file"
        sess = ort.InferenceSession(str(out_path), providers=["CPUExecutionProvider"])
        report = {"size_mb": size_mb, "ort_latency_ms_frames16_batch2": None,
                  "parity": {}}
        for batch in (1, 2):
            for frames in (SHORT_FRAMES, DOP_FRAMES):
                x = torch.randn(batch, frames, 180)
                with torch.no_grad():
                    ref = masknet(x)[-1].numpy()
                got = sess.run(["mask"], {"feats": x.numpy()})[0]
                a, b = ref.reshape(-1), got.reshape(-1)
                corr = float((a * b).sum() / (np.linalg.norm(a) * np.linalg.norm(b)))
                max_abs = float(np.abs(a - b).max())
                tag = f"b{batch}x{frames}"
                report["parity"][tag] = {"corr": corr, "max_abs": max_abs}
                print(f"parity {tag}: corr={corr:.8f} max_abs={max_abs:.3e}")
                assert corr > 0.9999, f"{tag}: corr {corr}"

        # CPU benchmark: the realtime steady-state job shape.
        x16 = torch.randn(2, SHORT_FRAMES, 180).numpy()
        for _ in range(3):
            sess.run(["mask"], {"feats": x16})
        times = []
        for _ in range(10):
            t0 = time.perf_counter()
            sess.run(["mask"], {"feats": x16})
            times.append((time.perf_counter() - t0) * 1000.0)
        times.sort()
        report["ort_latency_ms_frames16_batch2"] = times[len(times) // 2]
        print(f"ORT frames16 batch2 median: {times[len(times)//2]:.1f} ms "
              f"(min {times[0]:.1f}, max {times[-1]:.1f})")
        (out_path.parent / "dynamic_export_report.txt").write_text(
            repr(report) + "\n", encoding="utf-8")

    # -------------------------------------------------------------- resources
    res = args_cli.resource_dir
    golden = args_cli.golden_dir

    # Mel bank matrix exactly as the retired graph burned it in.
    import torchaudio.compliance.kaldi as kaldi
    mel, _ = kaldi.get_mel_banks(60, 2048, 48000, 20.0, 24000.0, 120.0, 23800.0, 1.0)
    mel_np = np.asarray(mel, dtype=np.float32)
    write_f32(res / "mel60_2048.f32", mel_np)
    write_f32(golden / "mel60_2048.f32", mel_np)
    print(f"mel matrix {mel_np.shape} -> {res}/mel60_2048.f32, golden copy")

    # DOP dither table: exact torch sequence of the retired graph.
    g = torch.Generator().manual_seed(DITHER_SEED)
    dop_dither = torch.randn(DOP_FRAMES, 1920, generator=g).numpy().astype(np.float32)
    write_f32(res / "dop_dither.f32", dop_dither)
    print(f"DOP dither {dop_dither.shape} -> {res}/dop_dither.f32")

    # ------------------------------------------------------- golden vectors
    # One second of deterministic 48 kHz audio; frames S = (24000-1920)/384+1.
    sr = 48000
    n = 24000
    t = np.arange(n, dtype=np.float64) / sr
    sig = (0.2 * np.sin(2 * np.pi * 220.0 * t)
           + 0.05 * np.sin(2 * np.pi * 3100.0 * t)).astype(np.float32)
    rng = np.random.default_rng(11)
    sig += (0.01 * rng.standard_normal(n)).astype(np.float32)
    S = (n - 1920) // 384 + 1
    write_f32(golden / "input_mono.f32", sig)
    (golden / "shapes.txt").write_text(
        f"n={n} sr={sr} frames={S}\n", encoding="utf-8")

    wav = torch.from_numpy(sig * MAX_WAV_VALUE).unsqueeze(0)  # int16 domain
    # Kaldi fbank reference, dither=0 (the C++ adds dither as a separate step).
    fbank = kaldi.fbank(wav, dither=0.0,
                        frame_length=ARGS.win_len / 48000 * 1000,
                        frame_shift=ARGS.win_inc / 48000 * 1000,
                        num_mel_bins=60, sample_frequency=48000,
                        window_type="hamming").numpy()  # [S, 60]
    assert fbank.shape == (S, 60), fbank.shape
    write_f32(golden / "fbank.f32", fbank)

    fbank_tr = torch.from_numpy(fbank).T
    d1 = torchaudio_delta(fbank_tr).numpy().T  # [S, 60]
    d2 = torchaudio_delta(torch.from_numpy(d1).T).numpy().T
    write_f32(golden / "delta.f32", d1)
    write_f32(golden / "delta_delta.f32", d2)
    feats = np.concatenate([fbank, d1, d2], axis=1).astype(np.float32)
    write_f32(golden / "feats.f32", feats)

    # STFT reference (center=False, matches the plugin pipeline).
    window = torch.hamming_window(1920, periodic=False)
    spec = torch.stft(torch.from_numpy(sig * MAX_WAV_VALUE), 1920, 384, 1920,
                      window=window, center=False, return_complex=False)
    # [961, S, 2] -> real, imag
    write_f32(golden / "stft_real.f32", spec[..., 0].numpy())
    write_f32(golden / "stft_imag.f32", spec[..., 1].numpy())

    # Mask gold from the real checkpoint on the golden feats.
    with torch.no_grad():
        mask_ref = masknet(torch.from_numpy(feats).unsqueeze(0))[-1][0].numpy()
    write_f32(golden / "mask.f32", mask_ref)  # [S, 961]

    # Masked iSTFT reference waveform (torch.istft, center=False).
    m = torch.from_numpy(mask_ref.T)                        # [961, S]
    masked_c = torch.complex(spec[..., 0] * m, spec[..., 1] * m)
    recon = torch.istft(masked_c, 1920, 384, 1920, window=window,
                        center=False, length=n).numpy()
    write_f32(golden / "istft_out.f32", recon / MAX_WAV_VALUE)

    # Short-window dither golden: 20 frames of the hash stream.
    dith = np.stack([short_dither_frame(i) for i in range(20)])
    write_f32(golden / "short_dither.f32", dith)

    # Digest file lets the C++ test verify it loaded the right gold set.
    h = hashlib.sha256()
    for name in ("mel60_2048.f32", "input_mono.f32", "fbank.f32", "delta.f32",
                 "delta_delta.f32", "feats.f32", "stft_real.f32",
                 "stft_imag.f32", "mask.f32", "istft_out.f32", "short_dither.f32"):
        h.update((golden / name).read_bytes())
    (golden / "golden.sha256").write_text(h.hexdigest() + "\n", encoding="utf-8")
    print(f"golden vectors -> {golden} (sha256 {h.hexdigest()[:16]}...)")
    return 0


def torchaudio_delta(spec_tr: torch.Tensor) -> torch.Tensor:
    """torchaudio.functional.compute_deltas replica used for golden output."""
    import torchaudio
    return torchaudio.functional.compute_deltas(spec_tr)


if __name__ == "__main__":
    raise SystemExit(main())
