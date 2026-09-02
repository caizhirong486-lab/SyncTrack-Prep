// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/PeakCompressor.h"
#include <cmath>

TEST_CASE ("PeakCompressor reduces peaks above threshold", "[compressor]")
{
    PeakCompressor comp;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    comp.prepare (spec);

    PeakCompressor::Params p;
    p.enabled = true;
    p.thresholdDb = -12.0f;
    p.ratio = 8.0f;
    p.attackMs = 1.0f;
    p.releaseMs = 50.0f;
    p.strength = 1.0f;
    p.makeupDb = 0.0f;
    comp.setParams (p);

    juce::AudioBuffer<float> buf (2, 48000);
    // 0.5s quiet then 0.5s loud
    for (int i = 0; i < 48000; ++i)
    {
        const float amp = i < 24000 ? 0.05f : 0.9f;
        const float s = amp * std::sin (2.0f * juce::MathConstants<float>::pi * 1000.0f * (float) i / 48000.0f);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s);
    }

    // Process whole buffer (stateful envelope carries across)
    comp.process (buf);

    float peakLoud = 0.0f;
    for (int i = 30000; i < 48000; ++i)
        peakLoud = juce::jmax (peakLoud, std::abs (buf.getSample (0, i)));

    // Input peak ~0.9; with 8:1 above -12dB (~0.25) should be well below 0.9
    REQUIRE (peakLoud < 0.55f);
    REQUIRE (comp.getLastGainReductionDb() < -0.5f);
}

TEST_CASE ("PeakCompressor near-unity for soft signal", "[compressor]")
{
    PeakCompressor comp;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    comp.prepare (spec);
    PeakCompressor::Params p;
    p.thresholdDb = -6.0f;
    p.ratio = 4.0f;
    p.strength = 1.0f;
    comp.setParams (p);

    juce::AudioBuffer<float> buf (1, 4096);
    for (int i = 0; i < 4096; ++i)
        buf.setSample (0, i, 0.05f * std::sin ((float) i * 0.1f));

    float inPeak = buf.getMagnitude (0, 0, 4096);
    comp.process (buf);
    float outPeak = buf.getMagnitude (0, 0, 4096);

    REQUIRE (std::abs (outPeak - inPeak) / inPeak < 0.05f);
}
