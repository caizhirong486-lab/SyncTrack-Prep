// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "TestTruePeak.h"
#include "dsp/ChannelRepair.h"
#include "dsp/NoiseSuppressor.h"
#include "dsp/Leveler.h"
#include "dsp/PeakCompressor.h"
#include "dsp/TruePeakLimiter.h"
#include <cmath>
#include <functional>

namespace
{
constexpr double sr = 48000.0;
constexpr float ceilingDb = -1.0f;
/** Judge accuracy is ~0.01 dB; the rest is headroom for gain-envelope ripple. */
constexpr float tpTolerance = 0.1f;

/** Raised-cosine edges. An abrupt burst edge is a step discontinuity whose own
    inter-sample overshoot swamps the measurement (it reads +0.5 dB high). */
float edgeFade (int i, int n, int fade)
{
    if (i < fade)
        return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) fade);
    if (i >= n - fade)
        return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (n - 1 - i) / (float) fade);
    return 1.0f;
}

juce::AudioBuffer<float> makeSine (float freq, float amp, int numSamples, int fade = 600)
{
    juce::AudioBuffer<float> buf (2, numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        const float w = edgeFade (i, numSamples, fade);
        const float s = amp * w * std::sin (2.0f * juce::MathConstants<float>::pi * freq * (float) i / (float) sr);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s * 0.9f);
    }
    return buf;
}

void runInBlocks (juce::AudioBuffer<float>& buf, int block,
                  const std::function<void (juce::AudioBuffer<float>&)>& fn)
{
    for (int off = 0; off < buf.getNumSamples(); off += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - off);
        juce::AudioBuffer<float> slice (buf.getNumChannels(), n);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, buf, ch, off, n);
        fn (slice);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.copyFrom (ch, off, slice, ch, 0, n);
    }
}
}

TEST_CASE ("True-peak judge is unbiased across the audio band", "[truepeak]")
{
    // Pins the measurement before it is trusted to judge the limiter.
    const float analytic = juce::Decibels::gainToDecibels (0.5f);
    for (float freq : { 997.0f, 5000.0f, 9000.0f, 13000.0f, 15000.0f, 19000.0f, 21000.0f })
    {
        auto buf = makeSine (freq, 0.5f, 6000);
        const float tp = measureTruePeakDb (buf, sr);
        INFO ("freq = " << freq << " Hz, measured " << tp << " dBTP, analytic " << analytic);
        REQUIRE (std::abs (tp - analytic) < 0.05f);
    }
}

TEST_CASE ("TruePeakLimiter holds -1 dBTP on inter-sample hostile content", "[truepeak][limiter]")
{
    TruePeakLimiter lim;
    juce::dsp::ProcessSpec spec { sr, 512, 2 };
    lim.prepare (spec);
    TruePeakLimiter::Params p;
    p.enabled = true;
    p.ceilingDb = ceilingDb;
    lim.setParams (p);

    // Near and above sr/4 is where inter-sample peaks exceed sample peaks most.
    for (float freq : { 5000.0f, 11500.0f, 15000.0f, 19000.0f })
    {
        lim.reset();
        auto buf = makeSine (freq, 0.999f, 24000);
        runInBlocks (buf, 512, [&lim] (juce::AudioBuffer<float>& b) { lim.process (b); });
        const float tp = measureTruePeakDb (buf, sr);
        INFO ("freq = " << freq << " Hz, measured " << tp << " dBTP");
        REQUIRE (tp <= ceilingDb + tpTolerance);
    }
}

TEST_CASE ("Full chain holds -1 dBTP with hot input", "[truepeak][chain]")
{
    juce::dsp::ProcessSpec spec { sr, 512, 2 };
    ChannelRepair cr;
    NoiseSuppressor ns;
    Leveler lv;
    PeakCompressor pc;
    TruePeakLimiter tp;
    cr.prepare (spec); ns.prepare (spec); lv.prepare (spec); pc.prepare (spec); tp.prepare (spec);

    ChannelRepair::Params crp;
    crp.enabled = true;
    crp.dialogueMono = true;
    cr.setParams (crp);

    NoiseSuppressor::Params nsp;
    nsp.enabled = false;
    ns.setParams (nsp);

    Leveler::Params lvp;
    lvp.enabled = true;
    lvp.strength = 0.75f;
    lvp.maxGainDb = 18.0f;
    lv.setParams (lvp);

    PeakCompressor::Params pcp;
    pcp.enabled = true;
    pcp.ratio = 3.0f;
    pcp.thresholdDb = -6.0f;
    pc.setParams (pcp);

    TruePeakLimiter::Params tpp;
    tpp.enabled = true;
    tpp.ceilingDb = ceilingDb;
    tp.setParams (tpp);

    // Quiet lead-in makes the leveler push gain up before the hot part arrives.
    const int n = 48000 * 2;
    juce::AudioBuffer<float> buf (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / (float) sr;
        const float amp = t < 1.0f ? 0.02f : 0.9f;
        const float w = edgeFade (i, n, 600);
        const float s = amp * w * std::sin (2.0f * juce::MathConstants<float>::pi * 9000.0f * t);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s * 0.8f);
    }

    runInBlocks (buf, 512, [&] (juce::AudioBuffer<float>& b)
    {
        cr.process (b);
        lv.process (b);
        ns.process (b);
        pc.process (b);
        tp.process (b);
    });

    const float measured = measureTruePeakDb (buf, sr);
    INFO ("chain measured " << measured << " dBTP");
    REQUIRE (measured <= ceilingDb + tpTolerance);
}
