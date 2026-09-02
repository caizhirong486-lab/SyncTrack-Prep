// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/TruePeakLimiter.h"
#include <cmath>

TEST_CASE ("TruePeakLimiter keeps sample peak under ceiling", "[limiter]")
{
    TruePeakLimiter lim;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    lim.prepare (spec);

    TruePeakLimiter::Params p;
    p.enabled = true;
    p.ceilingDb = -1.0f;
    p.releaseMs = 20.0f;
    lim.setParams (p);

    juce::AudioBuffer<float> buf (2, 2048);
    for (int i = 0; i < 2048; ++i)
    {
        const float s = 1.2f * std::sin (2.0f * juce::MathConstants<float>::pi * 2000.0f * (float) i / 48000.0f);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s * 0.9f);
    }

    lim.process (buf);

    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);
    float peak = 0.0f;
    for (int i = 0; i < 2048; ++i)
    {
        peak = juce::jmax (peak, std::abs (buf.getSample (0, i)));
        peak = juce::jmax (peak, std::abs (buf.getSample (1, i)));
    }

    REQUIRE (peak <= ceiling + 1.0e-4f);
}
