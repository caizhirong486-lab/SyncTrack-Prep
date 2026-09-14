// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DenoiseStage.h"

#include <array>
#include <memory>
#include <vector>

/**
 * Shared ClearerVoice frontend/backend for both MossFormer windows
 * (160 ms short window and 4 s DOP). All heavy constants are built once and
 * cached process-wide; the per-window work is plain float matmul, sized by
 * the frame count the caller asks for.
 *
 * Numeric contract (golden vectors: tests/golden/mossformer):
 *   fbank vs torchaudio kaldi (dither=0)   max abs err <= 2e-5
 *   STFT real/imag vs torch.stft           max abs err <= 1e-2 (int16 domain)
 *   iSTFT/OLA vs torch.istft               max abs err <= 7e-7
 * All stages run in the int16 amplitude domain (audio * 32768), like the
 * ClearerVoice reference decode.
 */
class MossFormerFrontend
{
public:
    static constexpr int kWinLen = 1920;    // STFT + fbank window (40 ms)
    static constexpr int kHop = 384;        // 8 ms
    static constexpr int kFft = 2048;       // zero-padded Kaldi fbank FFT
    static constexpr int kBins = 961;       // 1920/2 + 1
    static constexpr int kMel = 60;
    static constexpr int kFeat = 180;       // 60 fbank + delta + delta-delta

    enum class DitherMode { shortHash, dopTable };

    /** Immutable process-wide constants (windows, DFT bases, mel bank). */
    struct Constants
    {
        std::array<float, kWinLen> window {};        // torch hamming periodic=false
        std::vector<float> stftCos, stftSin;         // [961*1920], k-major
        std::vector<float> istftCos, istftSin;       // [1920*961], n-major
        std::array<float, kBins> binw {};            // DC/Nyquist 1, else 2
        std::vector<float> mel;                      // [60*1024], m-major

        int framesFor (int numSamples) const { return (numSamples - kWinLen) / kHop + 1; }
    };

    /** Loads the mel bank from <bundle>/mossformer2/mel60_2048.f32. Returns
        null and fills `error` when the resource is missing/corrupt. The
        DFT bases and window are computed in double and stored as float. */
    static std::shared_ptr<const Constants> loadConstants (const juce::File& melFile,
                                                           juce::String& error);

    // ---------------------------------------------------------------------

    struct Scratch
    {
        // fbank
        std::vector<float> frame, fbank, d1, d2, power;
        // stft
        std::vector<float> specRe, specIm, windowed;
        // istft
        std::vector<float> ola, olaNorm, frameOut;
        // shared window scratch
        std::vector<float> winScratch;
    };

    /** Dither plan for fbank frames (kaldi dither is per unfolded frame, so
        it must be injected frame-wise inside the fbank loop, before
        remove_dc_offset). */
    struct DitherPlan
    {
        DitherMode mode = DitherMode::shortHash;
        std::uint64_t frameBase = 0;                 // shortHash: absolute frame index
        const std::vector<float>* dopTable = nullptr; // dopTable: row per frame
    };

    /** Kaldi fbank of `frames` consecutive frames of normalised-domain audio
        (the int16-domain scaling happens inside, like the reference decode).
        Output feats [frames * 180] frame-major. With a plan, the kaldi dither
        is injected per frame; without, dither=0. */
    static void computeFeats (const Constants& c, Scratch& s,
                              const float* audio, int frames,
                              float* featsOut,
                              const DitherPlan* plan = nullptr);

    /** torchaudio-style deltas of [bins=60, time] with replicate edge pad,
        written frame-major [time * 60]. */
    static void computeDeltas (const Constants& c, Scratch& s,
                               const float* timeMajor /*[60*frames]*/, int frames,
                               std::vector<float>& out);

    /** 1920 STFT (center=False) of int16-domain audio; real/imag [frames*961]
        frame-major. `frames` = framesFor(numSamples). */
    static void computeSpec (const Constants& c, Scratch& s,
                             const float* audio, int numSamples,
                             std::vector<float>& re, std::vector<float>& im);

    /** Masked iSTFT/OLA over one full window: mask [frames*961] applied to
        the spec from computeSpec, output `frames*kHop + kWinLen` samples
        (the caller trims edges). */
    static void istftMasked (const Constants& c, Scratch& s,
                             const std::vector<float>& re,
                             const std::vector<float>& im,
                             const float* mask, int frames,
                             std::vector<float>& out);

    // ------------------------------------------------ shared window pipeline
    /** Builds [2, frames, 180] feats for both channels and runs the shared
        MaskNet -> mask [2, frames, 961]. `frameBase` is the fbank frame index
        of the window's frame 0 (absolute for shortHash; 0 for the DOP table,
        which restarts every window). */
    static bool buildFeatsAndRun (const Constants& c, Scratch& s,
                                  std::array<std::vector<float>, 2>& win,
                                  int frames, std::uint64_t frameBase,
                                  DitherMode mode,
                                  const std::vector<float>* dopTable,
                                  std::vector<float>& feats2,
                                  std::vector<float>& mask);

    /** STFT (normalised input, int16-domain internal scaling) -> mask ->
        iSTFT for both channels, amount crossfade against the window input,
        edge trim applied by memmove (first window: emitOffset 0 keeps the
        left edge). wet[ch][0..emitLen) is the emitted span. */
    static void renderWet (const Constants& c, Scratch& s,
                           std::array<std::vector<float>, 2>& win,
                           std::vector<float>& mask,
                           std::array<std::vector<float>, 2>& wet,
                           float amountWet, int frames, int winSamples,
                           int trim, int emitOffset);

    // ------------------------------------------------------- dither sources
    /** Short-window dither: deterministic counter hash (splitmix64 +
        Box-Muller) addressed by the generation-relative global fbank frame
        index. Golden-pinned in tests/golden/mossformer/short_dither.f32. */
    static void shortDitherFrame (std::uint64_t globalFrameIndex, float* out1920);

    /** DOP dither: the exact torch seed-20260906 sequence burned into the
        retired fixed graph, loaded from dop_dither.f32 [496*1920]. */
    static std::shared_ptr<const std::vector<float>> loadDopDither (const juce::File& file,
                                                                    juce::String& error);

private:
    static void rfftPower (const Constants& c, Scratch& s,
                           const float* frame, float* power1024);
};
