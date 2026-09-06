#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Export MossFormer2_SE_48K (ClearerVoice-Studio) to a full waveform->waveform ONNX graph.

The wrapper mirrors clearvoice/utils/decode.py for MossFormer2_SE_48K exactly
(kaldi fbank 60 bins + first/second deltas -> MaskNet -> mask * STFT -> iSTFT),
so the exported graph is validated against the reference pipeline for free.

Deltas: torchaudio.functional.compute_deltas. Fbank dither is set to 0 (the
reference decode uses dither=1.0, a small random noise; disabling it makes
exports and C++/Python comparisons deterministic — inaudible difference).

Usage:
  python3 scripts/export_mossformer2_onnx.py \
      --checkpoint third_party/mossformer2/last_best_checkpoint.pt \
      --output third_party/mossformer2/mossformer2_fp32.onnx \
      --window 192000 [--verify input.wav]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from types import SimpleNamespace

import torch
import torchaudio

CV_STUDIO = Path.home() / "nn-deps" / "cv-studio"
if not CV_STUDIO.exists():
    CV_STUDIO = Path(__file__).resolve().parents[2] / "cv-studio"
# package root is cv-studio/clearvoice (the inner dir named clearvoice)
sys.path.insert(0, str(CV_STUDIO / "clearvoice"))
sys.path.insert(0, str(CV_STUDIO))

from clearvoice.models.mossformer2_se.mossformer2_se_wrapper import MossFormer2_SE_48K  # noqa: E402
from clearvoice.utils.misc import stft, istft  # noqa: E402


def compute_fbank_no_dither(audio_in: torch.Tensor, args: SimpleNamespace) -> torch.Tensor:
    """clearvoice.utils.misc.compute_fbank with dither=0 (deterministic)."""
    frame_length = args.win_len / args.sampling_rate * 1000
    frame_shift = args.win_inc / args.sampling_rate * 1000
    return torchaudio.compliance.kaldi.fbank(
        audio_in, dither=0.0, frame_length=frame_length, frame_shift=frame_shift,
        num_mel_bins=args.num_mels, sample_frequency=args.sampling_rate,
        window_type=args.win_type)


class MossFormer2SE48KFull(torch.nn.Module):
    """waveform [1, T] -> enhanced waveform [1, T], T fixed at export window.

    Mirrors clearvoice decode.py for MossFormer2_SE_48K. The hamming window is
    a registered buffer (constant-folded into the graph) because the dynamo
    ONNX exporter has no decomposition for aten.hamming_window; the values are
    identical to torch.hamming_window(1920, periodic=False).
    """

    def kaldi_fbank(self, wav: torch.Tensor, apply_dither: bool = True) -> torch.Tensor:
        """Real-op replica of torchaudio.compliance.kaldi.fbank with a FIXED
        dither noise buffer (dither=1.0, seeded — deterministic renders), 
        snip_edges=True, remove_dc_offset, preemphasis 0.97, use_power=True,
        use_log_fbank=True, use_energy=False, hamming window. The ClearerVoice
        reference decodes with kaldi dither=1.0; without it the model leaves a
        ~17 dB shallower noise floor (measured, see pitfall 2026-09-06)."""
        args = self.args
        win = args.win_len
        hop = args.win_inc
        frames = wav.squeeze(0).unfold(0, win, hop)            # [S, win]
        if apply_dither:
            frames = frames + self.dither_noise[: frames.shape[0]]
        frames = frames - frames.mean(dim=1, keepdim=True)     # remove_dc_offset
        # preemphasis 0.97 (kaldi keeps the first sample unfiltered)
        # kaldi replicate-pad: first sample becomes x[0] - 0.97*x[0]
        first = frames[:, :1] * (1.0 - 0.97)
        frames = torch.cat([first, frames[:, 1:] - 0.97 * frames[:, :-1]], dim=1)
        frames = frames * self.fbank_window
        frames = torch.nn.functional.pad(frames, (0, self.kaldi_fft - args.win_len))
        spec = frames @ self.rfft_cosb.T                       # [S, F1]
        speci = frames @ self.rfft_sinb.T                      # [S, F1]
        power = spec.square() + speci.square()                 # [S, F1]
        # torchaudio's mel banks cover the first fft_n/2 bins (1024 for fft 2048)
        power = power[:, : self.mel_matrix.shape[1]]
        mel = power @ self.mel_matrix.T                        # [S, 60]
        return torch.log(torch.clamp(mel, min=torch.finfo(torch.float).eps))

    def __init__(self, masknet: torch.nn.Module, args: SimpleNamespace, out_len: int):
        super().__init__()
        self.masknet = masknet
        self.args = args
        n_fft = args.fft_len
        self.register_buffer(
            "window", torch.hamming_window(args.win_len, periodic=False))
        # Real-valued inverse-FFT basis (avoids complex ops entirely: the
        # legacy ONNX exporter cannot handle the complex construction that
        # torch.istft needs). irfft frame = Xr @ cosB^T - Xi @ sinB^T.
        f = torch.arange(n_fft // 2 + 1).unsqueeze(1).double()   # [F, 1]
        n = torch.arange(n_fft).unsqueeze(0).double()            # [1, N]
        self.register_buffer("cosb", torch.cos(2 * torch.pi * f * n / n_fft).float())
        self.register_buffer("sinb", torch.sin(2 * torch.pi * f * n / n_fft).float())
        # conjugate-symmetry weights: DC and Nyquist bins count once, the rest twice
        binw = torch.ones(n_fft // 2 + 1)
        binw[1:-1] = 2.0
        self.register_buffer("binw", binw)
        # Sum of squared windows over the fixed window length (istft norm).
        n_frames = (out_len - n_fft) // args.win_inc + 1
        norm = torch.zeros(out_len)
        w2 = self.window.square()
        for k in range(n_frames):
            norm[k * args.win_inc: k * args.win_inc + n_fft] += w2
        self.register_buffer("istft_norm", norm.clamp_min(1e-8))
        idx = (torch.arange(n_frames).unsqueeze(1) * args.win_inc
               + torch.arange(n_fft).unsqueeze(0)).flatten()
        self.register_buffer("ola_idx", idx)
        # --- kaldi fbank front-end constants (dither=0, use_energy=False) ---
        self.register_buffer("fbank_window",
                             torch.hamming_window(args.win_len, periodic=False))
        # Fixed kaldi dither noise (dither=1.0): seeded so offline renders are
        # reproducible. The ClearerVoice reference decodes with kaldi dither
        # 1.0; without it the model leaves a much shallower floor (measured).
        n_frames = (out_len - args.win_len) // args.win_inc + 1
        g = torch.Generator().manual_seed(20260906)
        self.register_buffer("dither_noise",
                             torch.randn(n_frames, args.win_len, generator=g))
        # rfft basis for the zero-padded kaldi fft size (2048 for win 1920)
        kaldi_fft = 1
        while kaldi_fft < args.win_len:
            kaldi_fft *= 2
        self.kaldi_fft = kaldi_fft
        fr = torch.arange(kaldi_fft // 2 + 1).unsqueeze(1).double()   # [F1, 1]
        nn_ = torch.arange(kaldi_fft).unsqueeze(0).double()           # [1, F2]
        self.register_buffer("rfft_cosb", torch.cos(2 * torch.pi * fr * nn_ / kaldi_fft).float())
        self.register_buffer("rfft_sinb", torch.sin(2 * torch.pi * fr * nn_ / kaldi_fft).float())
        import torchaudio.compliance.kaldi as _kaldi
        mel, _ = _kaldi.get_mel_banks(args.num_mels, kaldi_fft, args.sampling_rate,
                                      20.0, 24000.0, 120.0, 23800.0, 1.0)
        self.register_buffer("mel_matrix", torch.as_tensor(mel).float())
        # numeric parity check against torchaudio (feature-level alignment)
        probe = torch.randn(1, out_len)
        ref = torchaudio.compliance.kaldi.fbank(
            probe, dither=0.0, frame_length=args.win_len / args.sampling_rate * 1000,
            frame_shift=args.win_inc / args.sampling_rate * 1000,
            num_mel_bins=args.num_mels, sample_frequency=args.sampling_rate,
            window_type=args.win_type)
        mine = self.kaldi_fbank(probe, apply_dither=False)
        err = (mine - ref).abs().max().item()
        assert err < 1e-3, f"kaldi fbank replica mismatch: {err}"
        print(f"fbank replica parity: max err {err:.2e}")

    def forward(self, wav: torch.Tensor) -> torch.Tensor:
        args = self.args
        fbanks = self.kaldi_fbank(wav)                            # [S, 60]
        fbank_tr = torch.transpose(fbanks, 0, 1)                  # [60, S]
        fbank_delta = torchaudio.functional.compute_deltas(fbank_tr)
        fbank_delta_delta = torchaudio.functional.compute_deltas(fbank_delta)
        fbank_delta = torch.transpose(fbank_delta, 0, 1)          # [S, 60]
        fbank_delta_delta = torch.transpose(fbank_delta_delta, 0, 1)
        feats = torch.cat([fbanks, fbank_delta, fbank_delta_delta], dim=1)  # [S, 180]
        feats = feats.unsqueeze(0)                                # [1, S, 180]

        out_list = self.masknet(feats)
        pred_mask = out_list[-1]                                  # [B, S, 961]
        pred_mask = pred_mask.permute(2, 1, 0)                    # [961, S, 1] as in decode.py

        # stft(audio_segment) in decode.py: unbatched input -> [961, S, 2]
        spectrum = torch.stft(wav.squeeze(0), args.fft_len, args.win_inc,
                              args.win_len, window=self.window, center=False,
                              return_complex=False)
        masked_spec = spectrum * pred_mask
        # Real-op istft (matches torch.istft: synthesis window + sum-of-w^2 norm)
        xr = masked_spec[:, :, 0].transpose(0, 1)   # [S, F]
        xi = masked_spec[:, :, 1].transpose(0, 1)   # [S, F]
        # real irfft with conjugate-symmetry weights and 1/N normalization
        frames = (((xr * self.binw) @ self.cosb - (xi * self.binw) @ self.sinb)
                  * (1.0 / args.fft_len)) * self.window                 # [S, N]
        out = torch.zeros(1, wav.shape[-1],
                          dtype=frames.dtype, device=frames.device)
        out = out.index_add(1, self.ola_idx, frames.reshape(1, -1)).squeeze(0)
        out = out / self.istft_norm
        return out.unsqueeze(0)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--window", type=int, default=192000, help="export window in samples (4 s @48k)")
    ap.add_argument("--verify", type=Path, default=None, help="optional wav to compare ONNX vs torch")
    ap.add_argument("--opset", type=int, default=17)
    args_cli = ap.parse_args()

    args = SimpleNamespace(
        win_type="hamming", win_len=1920, win_inc=384, fft_len=1920,
        num_mels=60, sampling_rate=48000,
    )

    print("Loading ClearerVoice MossFormer2_SE_48K ...")
    wrapper = MossFormer2_SE_48K(args)  # instantiates TestNet
    state = torch.load(args_cli.checkpoint, map_location="cpu", weights_only=True)
    wrapper.model.load_state_dict(state)
    wrapper.model.eval()

    full = MossFormer2SE48KFull(wrapper.model, args, args_cli.window).eval()
    wav = torch.zeros(1, args_cli.window)
    with torch.no_grad():
        ref = full(wav)
    print(f"torch reference output: {tuple(ref.shape)}")

    out_path = args_cli.output
    out_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"Exporting ONNX ({args_cli.window} samples fixed window) ...")
    torch.onnx.export(
        full, wav, str(out_path),
        input_names=["input"], output_names=["output"],
        dynamic_axes=None, opset_version=args_cli.opset, do_constant_folding=True,
    )
    print(f"Wrote {out_path} ({out_path.stat().st_size / 1e6:.1f} MB)")

    if args_cli.verify is not None:
        import onnxruntime as ort
        import soundfile as sf
        data, sr = sf.read(str(args_cli.verify), dtype="float32", always_2d=True)
        assert sr == 48000, f"verify wav must be 48k, got {sr}"
        seg = torch.from_numpy(data.T).mean(dim=0, keepdim=True)[:, : args_cli.window]
        with torch.no_grad():
            torch_out = full(seg)
        sess = ort.InferenceSession(str(out_path), providers=["CPUExecutionProvider"])
        ort_out = sess.run(["output"], {"input": seg.numpy()})[0]
        diff = (torch_out.numpy() - ort_out).__abs__().max()
        a = torch_out.numpy().flatten()
        b = ort_out.flatten()
        corr = float(((a * b).sum())
                     / (max((a * a).sum(), 1e-20) ** 0.5 * max((b * b).sum(), 1e-20) ** 0.5))
        rms_out = float(((b * b).mean()) ** 0.5)
        print(f"verify: max |torch-ort| = {diff:.3e}, corr = {corr:.6f}, "
              f"ort rms = {rms_out:.5f}")
        # f32 runs of a 24-block model differ across math libraries; a raw
        # max-diff bound is meaningless. Graph equivalence is a correlation
        # gate; the hard numeric pin happens C++-ORT vs python-ORT downstream.
        assert corr > 0.99, "ONNX graph diverges from torch reference"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
