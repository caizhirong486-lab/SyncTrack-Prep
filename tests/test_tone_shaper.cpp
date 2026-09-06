// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/ToneShaper.h"
#include <cmath>
#include <random>

namespace
{
constexpr double sr = 48000.0;

juce::AudioBuffer<float> sineBuffer (double freq, float amp, double seconds)
{
    juce::AudioBuffer<float> buf (2, (int) (sr * seconds));
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const float t = (float) (i / sr);
        const float s = amp * std::sin (2.0f * juce::MathConstants<float>::pi * (float) freq * t);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s);
    }
    return buf;
}

float rmsDb (const juce::AudioBuffer<float>& b, int start, int len)
{
    double s = 0.0;
    for (int i = start; i < start + len; ++i)
        s += (double) b.getSample (0, i) * b.getSample (0, i);
    return juce::Decibels::gainToDecibels ((float) std::sqrt (s / (double) len), -120.0f);
}

/** RMS change (dB) the shaper applies to a steady tone, measured in the
    second half of the render so filter settling is out of the window. */
float processedRmsDb (double freq, float tone)
{
    ToneShaper ts;
    juce::dsp::ProcessSpec spec { sr, 512, 2 };
    ts.prepare (spec);
    ToneShaper::Params p;
    p.enabled = true;
    p.tone = tone;
    ts.setParams (p);

    auto buf = sineBuffer (freq, 0.5f, 1.0);
    ts.process (buf);
    return rmsDb (buf, (int) sr / 2, (int) sr / 2);
}
}

TEST_CASE ("ToneShaper at zero is transparent", "[tone]")
{
    ToneShaper ts;
    juce::dsp::ProcessSpec spec { sr, 512, 2 };
    ts.prepare (spec);
    ts.setParams ({ true, 0.0f });
    REQUIRE (ts.getLatencySamples() == 0);

    std::mt19937 rng (7);
    std::normal_distribution<float> noise (0.0f, 0.2f);
    juce::AudioBuffer<float> buf (2, 48000);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < buf.getNumSamples(); ++i)
            buf.setSample (ch, i, noise (rng));

    const auto reference (buf);
    ts.process (buf);

    // At tone = 0 every RBJ section here degenerates to H(z) = 1, so the
    // render must match the input bit-for-bit (the processor also short-circuits).
    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < buf.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (buf.getSample (ch, i) - reference.getSample (ch, i)));
    REQUIRE (worst < 1.0e-6f);
}

TEST_CASE ("ToneShaper bright side pushes 3 kHz over 200 Hz", "[tone]")
{
    const float low = processedRmsDb (200.0, 1.0f);
    const float high = processedRmsDb (3000.0, 1.0f);
    INFO ("200 Hz " << low << " dB, 3 kHz " << high << " dB");
    REQUIRE (high - low > 2.0f);
}

TEST_CASE ("ToneShaper dark side is the mirror", "[tone]")
{
    const float low = processedRmsDb (200.0, -1.0f);
    const float high = processedRmsDb (3000.0, -1.0f);
    INFO ("200 Hz " << low << " dB, 3 kHz " << high << " dB");
    REQUIRE (low - high > 2.0f);
}

TEST_CASE ("ToneShaper output stays finite at the extremes", "[tone]")
{
    for (float tone : { -1.0f, 1.0f })
    {
        ToneShaper ts;
        juce::dsp::ProcessSpec spec { sr, 512, 2 };
        ts.prepare (spec);
        ts.setParams ({ true, tone });

        std::mt19937 rng (11);
        std::normal_distribution<float> noise (0.0f, 0.9f); // hot, near full scale
        juce::AudioBuffer<float> buf (2, 48000);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                buf.setSample (ch, i, juce::jlimit (-1.0f, 1.0f, noise (rng)));

        ts.process (buf);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                REQUIRE (std::isfinite (buf.getSample (ch, i)));
    }
}
