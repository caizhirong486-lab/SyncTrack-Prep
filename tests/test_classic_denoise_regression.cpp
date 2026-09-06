// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/ClassicDenoise.h"
#include "dsp/NoiseSuppressor.h"
#include "dsp/Presets.h"
#include <random>

TEST_CASE ("ClassicDenoise adapter is bit-identical to the direct NoiseSuppressor", "[denoise][classic]")
{
    // Decision 7 pins "Classic unchanged": the adapter must not alter the
    // spectral path in any way (params, block handling, latency).
    ClassicDenoise adapted;
    NoiseSuppressor direct;

    adapted.prepare (48000.0, 512, 2);
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    direct.prepare (spec);

    auto chain = Presets::chainFor (Presets::clean, DenoiseMode::classic);
    auto params = chain.noiseSuppressor;
    params.amount = 0.55f; // the Amount knob overrides the preset seed
    adapted.setClassicParams (params);
    direct.setParams (params);

    REQUIRE (adapted.getLatencySamples() == direct.getLatencySamples());

    std::mt19937 rng (99);
    std::normal_distribution<float> dist (0.0f, 0.02f);
    juce::AudioBuffer<float> input (2, 48000);
    for (int i = 0; i < input.getNumSamples(); ++i)
    {
        const float n = dist (rng);
        const float s = i > 24000 ? 0.1f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * (float) (i - 24000) / 48000.0f) : 0.0f;
        input.setSample (0, i, s + n);
        input.setSample (1, i, s * 0.8f + n);
    }

    juce::AudioBuffer<float> outA (input), outD (input);
    const int block = 480; // not a multiple of the hop, on purpose
    for (int off = 0; off + block <= 48000; off += block)
    {
        juce::AudioBuffer<float> sa (2, block), sd (2, block);
        sa.copyFrom (0, 0, outA, 0, off, block);
        sa.copyFrom (1, 0, outA, 1, off, block);
        sd.copyFrom (0, 0, outD, 0, off, block);
        sd.copyFrom (1, 0, outD, 1, off, block);
        adapted.process (sa);
        direct.process (sd);
        outA.copyFrom (0, off, sa, 0, 0, block);
        outA.copyFrom (1, off, sa, 1, 0, block);
        outD.copyFrom (0, off, sd, 0, 0, block);
        outD.copyFrom (1, off, sd, 1, 0, block);
    }

    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 48000; ++i)
            worst = juce::jmax (worst,
                std::abs (outA.getSample (ch, i) - outD.getSample (ch, i)));
    INFO ("worst sample diff " << worst);
    REQUIRE (worst == 0.0f);
}
