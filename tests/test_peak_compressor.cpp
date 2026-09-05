// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/PeakCompressor.h"
#include <cmath>
#include <random>

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

namespace
{
juce::AudioBuffer<float> toneBuffer (double freq, float peakAmp, double seconds, double startSec = 0.0)
{
    juce::AudioBuffer<float> buf (2, (int) (48000.0 * seconds));
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const float t = (float) (startSec + i / 48000.0);
        const float s = peakAmp * std::sin (2.0f * juce::MathConstants<float>::pi * (float) freq * t);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s);
    }
    return buf;
}

PeakCompressor makeCompressor (float thresholdDb, float ratio)
{
    PeakCompressor comp;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    comp.prepare (spec);
    PeakCompressor::Params p;
    p.enabled = true;
    p.thresholdDb = thresholdDb;
    p.ratio = ratio;
    p.attackMs = 15.0f;
    p.releaseMs = 120.0f;
    p.transientReleaseMs = 40.0f;
    p.strength = 1.0f;
    p.makeupDb = 0.0f;
    comp.setParams (p);
    return comp;
}
}

TEST_CASE ("Scene level lowers the threshold and increases gain reduction", "[compressor]")
{
    // thresholdDb stays the legacy fallback; with a scene level the threshold
    // rides scene - 4 dB, so a quieter scene compresses the same signal
    // harder. Pins the coupling AND the clamp: the legacy -12 must not act as
    // a floor on the coupled threshold (that would flatten both cases to -3 dB).
    auto steadyGr = [] (float sceneDb)
    {
        auto comp = makeCompressor (-12.0f, 2.0f);
        comp.setSceneLevelDb (sceneDb);
        auto buf = toneBuffer (1000.0, 0.5f, 1.0); // -6 dBFS peak
        comp.process (buf);
        return comp.getLastGainReductionDb();
    };

    const float quietScene = steadyGr (-30.0f); // threshold -34 dB
    const float loudScene  = steadyGr (-10.0f); // threshold -14 dB
    INFO ("quiet scene GR " << quietScene << " dB, loud scene GR " << loudScene << " dB");
    REQUIRE (quietScene < loudScene);  // quieter scene -> more reduction
    REQUIRE (quietScene < -3.0f);      // genuinely engaged on the quiet scene
    REQUIRE (loudScene  < -0.5f);
}

TEST_CASE ("Without a scene level the legacy threshold applies unchanged", "[compressor]")
{
    // Fallback path: the compressor must behave exactly as before when the
    // caller never feeds it a scene level.
    auto comp = makeCompressor (-12.0f, 2.0f);
    // 100 Hz + fast attack so the peak envelope actually reaches the sine's
    // peak within each cycle (at 1 kHz a 15 ms follower only sees ~-7.3 dB).
    comp.setParams ([&]
    {
        auto p = PeakCompressor::Params{};
        p.thresholdDb = -12.0f;
        p.ratio = 2.0f;
        p.attackMs = 1.0f;
        p.releaseMs = 120.0f;
        p.transientReleaseMs = 40.0f;
        p.strength = 1.0f;
        return p;
    }());
    auto buf = toneBuffer (100.0, 0.5f, 0.5); // -6 dBFS peak
    comp.process (buf);
    // Envelope settles at -6 dB: 6 dB over a 2:1 threshold of -12 -> -3 dB GR.
    REQUIRE (std::abs (comp.getLastGainReductionDb() + 3.0f) < 0.5f);
}

TEST_CASE ("Transient content recovers faster than sustained tone", "[compressor]")
{
    // Probe method: exciter, 40 ms of silence, then a quiet probe well below
    // the threshold. The probe's gain reduction reads the envelope left over
    // from the exciter — spiky content sits in the 40 ms release, steady tone
    // stays on 120 ms, so the same peak level leaves more GR behind.
    auto run = [] (bool sustained)
    {
        PeakCompressor comp;
        juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
        comp.prepare (spec);
        PeakCompressor::Params p;
        p.enabled = true;
        p.thresholdDb = -20.0f;
        p.ratio = 4.0f;
        p.attackMs = 1.0f;
        p.releaseMs = 120.0f;
        p.transientReleaseMs = 40.0f;
        p.strength = 1.0f;
        comp.setParams (p);

        // Exciter at -6 dBFS peak: steady tone (crest ~4 dB -> sustained) or
        // continuous noise (crest ~10 dB -> transient mode).
        const int exciterLen = (int) (48000.0 * 0.3);
        const int gapLen = (int) (48000.0 * 0.04);
        juce::AudioBuffer<float> exciter (2, exciterLen + gapLen);
        exciter.clear();
        if (sustained)
        {
            const auto tone = toneBuffer (1000.0, 0.5f, 0.3);
            for (int ch = 0; ch < 2; ++ch)
                exciter.copyFrom (ch, 0, tone, ch, 0, exciterLen);
        }
        else
        {
            std::mt19937 rng (21);
            std::normal_distribution<float> noise (0.0f, 0.15f);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < exciterLen; ++i)
                    exciter.setSample (ch, i, noise (rng));
        }

        comp.process (exciter);

        auto probe = toneBuffer (1000.0, 0.063f, 0.02, 1.0); // ~-24 dBFS peak
        comp.process (probe);
        return comp.getLastGainReductionDb();
    };

    const float sustainedGr = run (true);
    const float transientGr = run (false);
    INFO ("sustained " << sustainedGr << " dB, transient " << transientGr << " dB");
    REQUIRE (sustainedGr < transientGr - 1.0f); // slower recovery -> deeper leftover GR
    REQUIRE (transientGr < -1.0f);              // exciter engaged the compressor
}
