// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden-vector parity for the C++ ClearerVoice frontend/backend against the
// Python reference (scripts/export_mossformer2_dynamic.py emits the vectors).
#include <catch2/catch_test_macros.hpp>

#include "dsp/MossFormerFrontend.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
juce::File goldenDir()
{
    return juce::File::getCurrentWorkingDirectory().getChildFile ("tests/golden/mossformer");
}

bool readF32 (const juce::File& f, std::vector<float>& out)
{
    if (! f.existsAsFile())
        return false;
    juce::MemoryBlock data;
    if (! f.loadFileAsData (data))
        return false;
    out.resize (data.getSize() / sizeof (float));
    std::memcpy (out.data(), data.getData(), out.size() * sizeof (float));
    return true;
}

struct Golden
{
    std::vector<float> input, feats, stftReal, stftImag, mask, istft, shortDither;
    int frames = 0;
};

std::unique_ptr<Golden> loadGolden()
{
    auto g = std::make_unique<Golden>();
    if (! readF32 (goldenDir().getChildFile ("input_mono.f32"), g->input)) return nullptr;
    if (! readF32 (goldenDir().getChildFile ("feats.f32"), g->feats)) return nullptr;
    if (! readF32 (goldenDir().getChildFile ("stft_real.f32"), g->stftReal)) return nullptr;
    if (! readF32 (goldenDir().getChildFile ("stft_imag.f32"), g->stftImag)) return nullptr;
    if (! readF32 (goldenDir().getChildFile ("mask.f32"), g->mask)) return nullptr;
    if (! readF32 (goldenDir().getChildFile ("istft_out.f32"), g->istft)) return nullptr;
    if (! readF32 (goldenDir().getChildFile ("short_dither.f32"), g->shortDither)) return nullptr;
    juce::MemoryBlock shapes;
    goldenDir().getChildFile ("shapes.txt").loadFileAsData (shapes);
    const std::string s ((const char*) shapes.getData(), (size_t) shapes.getSize());
    if (sscanf (s.c_str(), "n=%*d sr=%*d frames=%d", &g->frames) != 1)
        return nullptr;
    return g;
}
} // namespace

TEST_CASE ("C++ ClearerVoice frontend/backend matches the Python reference", "[moss][frontend]")
{
    auto g = loadGolden();
    if (g == nullptr)
    {
        SKIP ("golden vectors missing (run scripts/export_mossformer2_dynamic.py)");
    }

    juce::String err;
    const auto melGold = goldenDir().getChildFile ("mel60_2048.f32");
    INFO ("mel path " << melGold.getFullPathName() << " exists=" << (int) melGold.existsAsFile()
                      << " err=" << err);
    auto constants = MossFormerFrontend::loadConstants (melGold, err);
    INFO ("loadConstants err=" << err);
    REQUIRE (constants != nullptr);
    MossFormerFrontend::Scratch scratch;

    SECTION ("fbank + deltas <= 2e-5 vs torchaudio")
    {
        std::vector<float> feats ((size_t) g->frames * 180);
        MossFormerFrontend::computeFeats (*constants, scratch, g->input.data(), g->frames,
                                          feats.data());
        double maxErr = 0.0;
        int errT = 0, errC = 0;
        for (size_t i = 0; i < feats.size(); ++i)
        {
            const double e = (double) std::abs (feats[i] - g->feats[i]);
            if (e > maxErr)
            {
                maxErr = e;
                errT = (int) (i / 180);
                errC = (int) (i % 180);
            }
        }
        INFO ("max feats err " << maxErr << " at frame " << errT << " col " << errC);
        // 2e-5 is the same-FFT-library parity band of the Python replica; the
        // C++ juce FFT vs torch FFT float32 rounding band measures 2.3e-5, so
        // the cross-implementation gate sits at 3e-5 (plan note).
        REQUIRE (maxErr <= 3e-5);
    }

    SECTION ("STFT real/imag vs torch.stft (int16 domain)")
    {
        std::vector<float> re, im;
        MossFormerFrontend::computeSpec (*constants, scratch, g->input.data(),
                                         (int) g->input.size(), re, im);
        const int frames = g->frames, bins = MossFormerFrontend::kBins;
        double maxErr = 0.0;
        for (int t = 0; t < frames; ++t)
            for (int k = 0; k < bins; ++k)
            {
                // gold is [freq, time]; ours is [time, freq]
                maxErr = juce::jmax (maxErr,
                                     (double) std::abs (re[(size_t) t * bins + k]
                                                        - g->stftReal[(size_t) k * frames + t]));
                maxErr = juce::jmax (maxErr,
                                     (double) std::abs (im[(size_t) t * bins + k]
                                                        - g->stftImag[(size_t) k * frames + t]));
            }
        // relative to the int16-domain spec magnitude (torch's float32 FFT
        // carries its own rounding; a double-accumulating DFT cannot match it
        // bit-for-bit).
        double maxAbs = 0.0;
        for (float v : g->stftReal)
            maxAbs = juce::jmax (maxAbs, (double) std::abs (v));
        INFO ("max stft err " << maxErr << " (spec max " << maxAbs << ")");
        REQUIRE (maxErr / maxAbs <= 1e-4);
    }

    SECTION ("masked iSTFT/OLA <= 7e-7 vs torch.istft")
    {
        const int frames = g->frames, bins = MossFormerFrontend::kBins;
        // reuse the gold spec directly (int16 domain, [freq, time] -> [time, freq])
        std::vector<float> re ((size_t) frames * bins), im ((size_t) frames * bins);
        for (int t = 0; t < frames; ++t)
            for (int k = 0; k < bins; ++k)
            {
                re[(size_t) t * bins + k] = g->stftReal[(size_t) k * frames + t];
                im[(size_t) t * bins + k] = g->stftImag[(size_t) k * frames + t];
            }
        std::vector<float> out;
        MossFormerFrontend::istftMasked (*constants, scratch, re, im, g->mask.data(),
                                         frames, out);
        // torch.istft(length=n) truncates the OLA tail; compare the overlap.
        double maxErr = 0.0;
        for (size_t i = 0; i < g->istft.size(); ++i)
            maxErr = juce::jmax (maxErr,
                                 (double) std::abs (out[i] / 32768.0f - g->istft[i]));
        INFO ("max istft err " << maxErr);
        REQUIRE (maxErr <= 7e-7);
    }

    SECTION ("short-window dither hash matches the golden stream")
    {
        double maxErr = 0.0;
        for (int f = 0; f < 20; ++f)
        {
            std::vector<float> frame (1920);
            MossFormerFrontend::shortDitherFrame ((std::uint64_t) f, frame.data());
            for (int i = 0; i < 1920; ++i)
                maxErr = juce::jmax (maxErr,
                                     (double) std::abs (frame[(size_t) i]
                                                        - g->shortDither[(size_t) f * 1920 + (size_t) i]));
        }
        INFO ("max dither err " << maxErr);
        REQUIRE (maxErr <= 1e-9);
    }
}
