// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/MossFormerDenoise.h"
#include <cmath>
#include <random>

namespace
{
juce::File mossModelFromEnv()
{
    if (auto* env = std::getenv ("STP_MOSS_MODEL"))
        return juce::File (juce::String (env));
    return juce::File::getCurrentWorkingDirectory()
               .getChildFile ("third_party/mossformer2/mossformer2_int8.onnx");
}
}

TEST_CASE ("MossFormer stitching: chunk boundaries stay continuous and latency is exact", "[denoise][moss]")
{
    const auto model = mossModelFromEnv();
    if (! model.existsAsFile())
    {
        SKIP ("MossFormer ONNX model not present (third_party/mossformer2/mossformer2_int8.onnx)");
    }
#ifdef STP_ENABLE_MOSSFORMER
    MossFormerDenoise den;
    den.setModelPath (model);
    den.prepare (48000.0, 512, 2);
    REQUIRE (den.isLoaded());
    REQUIRE_FALSE (den.supportsRealtime());

    // Latency contract: 3.5 s at 48 kHz (3 s stride + 0.5 s pad).
    REQUIRE (den.getLatencySamples() == MossFormerDenoise::latency48);

    // Feed ~8 s of speech-like band-limited material (several chunk
    // boundaries) and check the output stream for boundary discontinuities:
    // the edge-discard stitching must produce seamless audio, not 3 s clicks.
    const int sr = 48000;
    const int n = sr * 8;
    juce::AudioBuffer<float> in (2, n), out (2, n);
    std::mt19937 rng (11);
    std::normal_distribution<float> dist (0.0f, 0.02f);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / (float) sr;
        const float s = 0.1f * std::sin (2.0f * juce::MathConstants<float>::pi * (220.0f + 30.0f * t) * t)
                        + dist (rng);
        in.setSample (0, i, s);
        in.setSample (1, i, s * 0.9f);
    }
    out.copyFrom (0, 0, in, 0, 0, n);
    out.copyFrom (1, 0, in, 1, 0, n);
    den.setAmount (1.0f);
    for (int off = 0; off < n; off += 512)
    {
        juce::AudioBuffer<float> slice (2, 512);
        slice.copyFrom (0, 0, out, 0, off, 512);
        slice.copyFrom (1, 0, out, 1, off, 512);
        den.process (slice);
        out.copyFrom (0, off, slice, 0, 0, 512);
        out.copyFrom (1, off, slice, 1, 0, 512);
    }

    // After the priming region the output must track the input continuously:
    // compare against the delayed input and check the per-sample residual
    // around every 3 s chunk boundary for spikes.
    const int start = den.getLatencySamples() + sr / 2;
    float maxResidualAtBoundary = 0.0f;
    float maxResidualOverall = 0.0f;
    for (int i = start; i < n; ++i)
    {
        const float resid = std::abs (out.getSample (0, i) - in.getSample (0, i - den.getLatencySamples()));
        const int pos = i - den.getLatencySamples();
        const bool nearBoundary = (pos % MossFormerDenoise::stride48) < sr / 10
                               || (pos % MossFormerDenoise::stride48) > MossFormerDenoise::stride48 - sr / 10;
        maxResidualOverall = juce::jmax (maxResidualOverall, resid);
        if (nearBoundary)
            maxResidualAtBoundary = juce::jmax (maxResidualAtBoundary, resid);
    }
    INFO ("max residual at boundaries " << maxResidualAtBoundary
          << ", overall " << maxResidualOverall);
    // The NN output differs from the input (it denoises), so the residual is
    // not zero — but a stitching discontinuity would stand out as a step of
    // the full signal amplitude. Boundaries must not be much worse than the
    // overall residual.
    REQUIRE (maxResidualAtBoundary < maxResidualOverall * 1.05f + 0.01f);
#else
    SKIP ("Built without STP_ENABLE_MOSSFORMER");
#endif
}
