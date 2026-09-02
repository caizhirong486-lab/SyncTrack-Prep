// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/ChannelRepair.h"
#include <cmath>
#include <vector>

namespace
{
float hfProxy (const std::vector<float>& x)
{
    double d = 0.0, e = 0.0;
    for (size_t i = 1; i < x.size(); ++i)
    {
        const double diff = (double) x[i] - (double) x[i - 1];
        d += diff * diff;
        e += (double) x[i] * (double) x[i];
    }
    return (float) (d / (e + 1e-20));
}

juce::AudioBuffer<float> makeUncorrStereo (int n)
{
    juce::AudioBuffer<float> buf (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / 48000.0f;
        buf.setSample (0, i, 0.25f * std::sin (2.0f * juce::MathConstants<float>::pi * 300.0f * t));
        buf.setSample (1, i, 0.20f * std::sin (2.0f * juce::MathConstants<float>::pi * 700.0f * t + 1.3f));
    }
    return buf;
}

juce::AudioBuffer<float> makeLOnly (int n, float amp = 0.25f)
{
    juce::AudioBuffer<float> buf (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / 48000.0f;
        buf.setSample (0, i, amp * std::sin (2.0f * juce::MathConstants<float>::pi * 400.0f * t));
        buf.setSample (1, i, 0.0f);
    }
    return buf;
}

float rmsCh (const juce::AudioBuffer<float>& b, int ch, int start, int len)
{
    double s = 0.0;
    for (int i = start; i < start + len; ++i)
    {
        const float x = b.getSample (ch, i);
        s += (double) x * (double) x;
    }
    return (float) std::sqrt (s / (double) juce::jmax (1, len));
}
}

TEST_CASE ("ChannelRepair: L-only fills R without HF blow-up", "[channel][noise]")
{
    ChannelRepair cr;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    cr.prepare (spec);
    ChannelRepair::Params p;
    p.dialogueMono = true;
    cr.setParams (p);

    auto buf = makeLOnly (48000);
    std::vector<float> before (48000);
    for (int i = 0; i < 48000; ++i)
        before[(size_t) i] = buf.getSample (0, i);
    const float hf0 = hfProxy (before);

    cr.process (buf);

    REQUIRE (rmsCh (buf, 1, 1000, 40000) > 0.05f);
    REQUIRE (std::abs (rmsCh (buf, 0, 1000, 40000) - rmsCh (buf, 1, 1000, 40000))
             / rmsCh (buf, 0, 1000, 40000) < 0.05f);

    std::vector<float> after (48000);
    for (int i = 0; i < 48000; ++i)
        after[(size_t) i] = 0.5f * (buf.getSample (0, i) + buf.getSample (1, i));
    const float hf1 = hfProxy (after);
    REQUIRE (hf1 < hf0 * 1.25f + 0.01f);
}

TEST_CASE ("ChannelRepair: uncorrelated stereo must NOT mid-sum into HF noise", "[channel][noise]")
{
    ChannelRepair cr;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    cr.prepare (spec);
    ChannelRepair::Params p;
    p.dialogueMono = true;
    p.sideFightRatio = 0.5f;
    cr.setParams (p);

    auto buf = makeUncorrStereo (48000);
    std::vector<float> midIn (48000), midOut (48000);
    for (int i = 0; i < 48000; ++i)
        midIn[(size_t) i] = 0.5f * (buf.getSample (0, i) + buf.getSample (1, i));
    const float hfMidSum = hfProxy (midIn); // what OLD forceMono did

    cr.process (buf);

    for (int i = 0; i < 48000; ++i)
        midOut[(size_t) i] = 0.5f * (buf.getSample (0, i) + buf.getSample (1, i));
    const float hfOut = hfProxy (midOut);

    // Must be dual-mono (L≈R)
    float maxDiff = 0.0f;
    for (int i = 2000; i < 10000; ++i)
        maxDiff = juce::jmax (maxDiff, std::abs (buf.getSample (0, i) - buf.getSample (1, i)));
    REQUIRE (maxDiff < 1.0e-5f);

    // Must NOT be as harsh as mid-sum of uncorrelated inputs
    REQUIRE (hfOut < hfMidSum * 0.95f + 0.002f);
    // And stay near single-channel HF
    std::vector<float> lonly (48000);
    for (int i = 0; i < 48000; ++i)
        lonly[(size_t) i] = 0.25f * std::sin (2.0f * juce::MathConstants<float>::pi * 300.0f * (float) i / 48000.0f);
    REQUIRE (hfOut < hfProxy (lonly) * 1.5f + 0.01f);
}

TEST_CASE ("ChannelRepair auto: L-only becomes dual-mono", "[channel]")
{
    ChannelRepair cr;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    cr.prepare (spec);
    ChannelRepair::Params p;
    p.dialogueMono = false;
    p.activityDb = -60.0f;
    cr.setParams (p);

    auto buf = makeLOnly (24000);
    const int block = 512;
    for (int off = 0; off + block <= 24000; off += block)
    {
        juce::AudioBuffer<float> slice (2, block);
        slice.copyFrom (0, 0, buf, 0, off, block);
        slice.copyFrom (1, 0, buf, 1, off, block);
        cr.process (slice);
        buf.copyFrom (0, off, slice, 0, 0, block);
        buf.copyFrom (1, off, slice, 1, 0, block);
    }
    REQUIRE (rmsCh (buf, 1, 8000, 8000) > 0.05f);
}
