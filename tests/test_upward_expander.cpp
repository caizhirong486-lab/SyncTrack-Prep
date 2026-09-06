// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/UpwardExpander.h"
#include <cmath>

namespace
{
double rmsDb (const juce::AudioBuffer<float>& b, int start, int len)
{
    double s = 0.0;
    for (int i = start; i < start + len; ++i)
        s += (double) b.getSample (0, i) * b.getSample (0, i);
    return juce::Decibels::gainToDecibels ((float) std::sqrt (s / (double) len), -120.0f);
}
}

TEST_CASE ("UpwardExpander lifts quiet passages up to range and passes loud passages", "[expander]")
{
    UpwardExpander ue;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    ue.prepare (spec);

    UpwardExpander::Params p;
    p.enabled = true;
    p.ratio = 1.5f;
    p.rangeDb = 8.0f;
    p.attackMs = 5.0f;
    p.releaseMs = 150.0f;
    p.sceneOffsetDb = -20.0f;
    ue.setParams (p);

    const int sr = 48000;
    const int quietLen = sr;            // 1 s deep below threshold
    const int loudLen = sr;
    juce::AudioBuffer<float> buf (2, quietLen + loudLen);
    for (int i = 0; i < quietLen + loudLen; ++i)
    {
        const float amp = i < quietLen ? 0.002f : 0.2f;
        const float s = amp * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * (float) i / (float) sr);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s);
    }
    // Scene level far above both, so the quiet section sits deep below thr.
    ue.setSceneLevelDb (-10.0f);

    const int block = 512;
    for (int off = 0; off < buf.getNumSamples(); off += block)
    {
        const int len = juce::jmin (block, buf.getNumSamples() - off);
        juce::AudioBuffer<float> slice (2, len);
        slice.copyFrom (0, 0, buf, 0, off, len);
        slice.copyFrom (1, 0, buf, 0, off, len);
        ue.process (slice);
        buf.copyFrom (0, off, slice, 0, 0, len);
        buf.copyFrom (1, off, slice, 1, 0, len);
    }

    const float quietDb = rmsDb (buf, (int) (0.5 * sr), (int) (0.4 * sr));
    const float loudDb = rmsDb (buf, quietLen + (int) (0.5 * sr), (int) (0.4 * sr));
    // RMS reading: a 0.002 sine is -57 dBFS RMS and 0.2 is -17 dBFS RMS.
    // Quiet sits ~47 dB below the threshold (-30 dB): full +8 dB boost.
    // Loud is above threshold: unity.
    INFO ("quiet " << quietDb << " loud " << loudDb);
    REQUIRE (quietDb > -57.0f + 6.0f);
    REQUIRE (loudDb < -17.0f + 0.5f);
}

TEST_CASE ("UpwardExpander disabled or zero range is transparent", "[expander]")
{
    for (bool disabled : { true, false })
    {
        UpwardExpander ue;
        juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
        ue.prepare (spec);
        UpwardExpander::Params p;
        p.enabled = ! disabled;
        p.rangeDb = disabled ? 8.0f : 0.0f;
        ue.setParams (p);
        ue.setSceneLevelDb (-18.0f);

        juce::AudioBuffer<float> buf (2, 48000);
        for (int i = 0; i < 48000; ++i)
        {
            const float s = 0.05f * std::sin (2.0f * juce::MathConstants<float>::pi * 300.0f * (float) i / 48000.0f);
            buf.setSample (0, i, s);
            buf.setSample (1, i, s);
        }
        for (int off = 0; off < 48000; off += 512)
        {
            const int len = juce::jmin (512, 48000 - off);
            juce::AudioBuffer<float> slice (2, len);
            slice.copyFrom (0, 0, buf, 0, off, len);
            slice.copyFrom (1, 0, buf, 0, off, len);
            ue.process (slice);
            buf.copyFrom (0, off, slice, 0, 0, len);
            buf.copyFrom (1, off, slice, 1, 0, len);
        }
        float worst = 0.0f;
        for (int i = 4800; i < 48000; ++i) // skip attack region
            worst = juce::jmax (worst, std::abs (buf.getSample (0, i)
                - 0.05f * std::sin (2.0f * juce::MathConstants<float>::pi * 300.0f * (float) i / 48000.0f)));
        INFO ("disabled " << (int) disabled << " worst " << worst);
        REQUIRE (worst < 1.0e-4f);
    }
}
