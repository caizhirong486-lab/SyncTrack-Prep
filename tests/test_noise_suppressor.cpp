// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/NoiseSuppressor.h"
#include <cmath>
#include <random>

TEST_CASE ("NoiseSuppressor HPF when denoise off still can run disabled path", "[denoise]")
{
    NoiseSuppressor ns;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    ns.prepare (spec);

    NoiseSuppressor::Params p;
    p.enabled = false;
    p.amount = 0.0f;
    p.hpfHz = 200.0f;
    ns.setParams (p);

    juce::AudioBuffer<float> buf (2, 48000);
    for (int i = 0; i < 48000; ++i)
    {
        const float s = 0.3f * std::sin (2.0f * juce::MathConstants<float>::pi * 40.0f * (float) i / 48000.0f);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s);
    }
    // disabled → no change from HPF either (doHpf false when !enabled && amount low)
    ns.process (buf);
    REQUIRE (buf.getRMSLevel (0, 8000, 40000) > 0.05f);
}

TEST_CASE ("NoiseSuppressor denoise reduces stationary noise energy", "[denoise]")
{
    NoiseSuppressor ns;
    juce::dsp::ProcessSpec spec { 48000.0, 256, 2 };
    ns.prepare (spec);

    NoiseSuppressor::Params p;
    p.enabled = true;
    p.amount = 0.85f;
    p.hpfHz = 40.0f;
    p.overSubtract = 1.4f;
    ns.setParams (p);

    std::mt19937 rng (7);
    std::normal_distribution<float> dist (0.0f, 0.03f);
    juce::AudioBuffer<float> buf (2, 48000 * 3);
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const float n = dist (rng);
        buf.setSample (0, i, n);
        buf.setSample (1, i, n);
    }

    const float inRms = buf.getRMSLevel (0, 48000, 48000);
    // process full buffer in blocks
    const int block = 256;
    for (int off = 0; off + block <= buf.getNumSamples(); off += block)
    {
        juce::AudioBuffer<float> slice (2, block);
        slice.copyFrom (0, 0, buf, 0, off, block);
        slice.copyFrom (1, 0, buf, 1, off, block);
        ns.process (slice);
        buf.copyFrom (0, off, slice, 0, 0, block);
        buf.copyFrom (1, off, slice, 1, 0, block);
    }
    const float outRms = buf.getRMSLevel (0, 48000 * 2, 48000);
    REQUIRE (outRms < inRms * 0.85f);
}

TEST_CASE ("NoiseSuppressor reported latency matches the measured delay", "[denoise]")
{
    NoiseSuppressor ns;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    ns.prepare (spec);
    NoiseSuppressor::Params p;
    p.enabled = false;
    ns.setParams (p);
    // The latency is fixed: toggling Denoise must not change what the host
    // compensates for.
    REQUIRE (ns.getLatencySamples() == NoiseSuppressor::latencyWhenEnabled);

    // An impulse must come out exactly `reported` samples late in every mode
    // (disabled, enabled, near-zero amount). A wrong report here means the
    // host compensates by the wrong amount and the track drifts.
    for (bool enabled : { false, true })
    {
        p.enabled = enabled;
        p.amount = 0.5f;
        ns.setParams (p);
        ns.reset();

        if (enabled)
        {
            // Near-zero amount keeps the delay too (no bypass).
            p.amount = 0.005f;
            ns.setParams (p);
        }
        else
        {
            p.amount = 0.005f;
            ns.setParams (p);
        }

        const int n = 4096;
        const int impulseAt = 1000;
        juce::AudioBuffer<float> buf (2, n);
        buf.clear();
        buf.setSample (0, impulseAt, 1.0f);
        buf.setSample (1, impulseAt, 1.0f);

        const int block = 512;
        for (int off = 0; off < n; off += block)
        {
            juce::AudioBuffer<float> slice (2, block);
            for (int ch = 0; ch < 2; ++ch)
                slice.copyFrom (ch, 0, buf, ch, off, block);
            ns.process (slice);
            for (int ch = 0; ch < 2; ++ch)
                buf.copyFrom (ch, off, slice, ch, 0, block);
        }

        int peakIdx = 0;
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (buf.getSample (0, i));
            if (a > peak)
            {
                peak = a;
                peakIdx = i;
            }
        }

        INFO ("enabled " << enabled << ": impulse in at " << impulseAt
              << ", out at " << peakIdx << ", reported " << ns.getLatencySamples());
        REQUIRE (peakIdx - impulseAt == ns.getLatencySamples());
    }
}

namespace
{
/** Clean-preset denoise over a steady noise floor, optionally with a sustained
    tone standing in for voiced speech. Returns the level change in the measured
    window, in dB. */
float denoiseChangeDb (bool withVoice)
{
    NoiseSuppressor ns;
    const int block = 512;
    juce::dsp::ProcessSpec spec { 48000.0, (juce::uint32) block, 2 };
    ns.prepare (spec);

    NoiseSuppressor::Params p;
    p.enabled = true;
    p.amount = 0.55f;         // Clean preset values
    p.hpfHz = 70.0f;
    p.overSubtract = 1.05f;
    p.speechProtect = 0.75f;
    ns.setParams (p);

    const int n = 48000 * 4;
    juce::AudioBuffer<float> wet (2, n), dry (2, n);
    std::mt19937 rng (5);
    std::normal_distribution<float> nd (0.0f, 0.02f);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / 48000.0f;
        float s = nd (rng);
        if (withVoice && t > 1.0f)
            s += 0.15f * std::sin (2.0f * juce::MathConstants<float>::pi * 400.0f * t);
        wet.setSample (0, i, s); wet.setSample (1, i, s);
        dry.setSample (0, i, s); dry.setSample (1, i, s);
    }

    for (int off = 0; off < n; off += block)
    {
        const int len = juce::jmin (block, n - off);
        juce::AudioBuffer<float> slice (2, len);
        for (int ch = 0; ch < 2; ++ch)
            slice.copyFrom (ch, 0, wet, ch, off, len);
        ns.process (slice);
        for (int ch = 0; ch < 2; ++ch)
            wet.copyFrom (ch, off, slice, ch, 0, len);
    }

    const int start = 48000 * 2, len = 48000;
    const float in = dry.getRMSLevel (0, start, len);
    const float out = wet.getRMSLevel (0, start, len);
    return juce::Decibels::gainToDecibels (out / juce::jmax (in, 1.0e-9f));
}
}

TEST_CASE ("Denoise pulls a steady floor down by at least 4 dB", "[denoise]")
{
    // metric C at unit level. This has to be measured against the dry
    // signal, not just "output is quieter": a broken overlap-add once lost
    // 5.8 dB across the whole band and looked like noise reduction.
    const float change = denoiseChangeDb (false);
    INFO ("steady-noise change " << change << " dB");
    REQUIRE (change <= -4.0f);
}

TEST_CASE ("Denoise leaves voiced content alone", "[denoise]")
{
    // The noise estimate must not learn sustained voiced energy, otherwise it
    // subtracts speech from itself.
    const float change = denoiseChangeDb (true);
    INFO ("voice-plus-noise change " << change << " dB");
    REQUIRE (change > -1.0f);
}

TEST_CASE ("Denoise learns a noise floor that appears mid-stream", "[denoise]")
{
    // The exact failure on real material: the clip opens with near-silence,
    // so the old running-minimum locked itself to the silence floor and the
    // gate never let the estimate rise to the noise that starts seconds
    // later. Leading silence must not lock the gate, and the estimate must
    // engage within ~1-2 s of the noise arriving.
    NoiseSuppressor ns;
    const int block = 512;
    juce::dsp::ProcessSpec spec { 48000.0, (juce::uint32) block, 2 };
    ns.prepare (spec);

    NoiseSuppressor::Params p;
    p.enabled = true;
    p.amount = 0.55f;   // Clean preset values
    p.hpfHz = 70.0f;
    p.overSubtract = 1.05f;
    p.speechProtect = 0.75f;
    ns.setParams (p);

    const int n = 48000 * 6;
    juce::AudioBuffer<float> wet (2, n), dry (2, n);
    std::mt19937 rng (11);
    std::normal_distribution<float> nd (0.0f, 0.02f);
    for (int i = 0; i < n; ++i)
    {
        float s = 0.0f;
        if (i >= 48000 * 2) // 2 s of silence first
            s = nd (rng);   // steady noise from 2 s on
        wet.setSample (0, i, s); wet.setSample (1, i, s);
        dry.setSample (0, i, s); dry.setSample (1, i, s);
    }

    for (int off = 0; off < n; off += block)
    {
        juce::AudioBuffer<float> slice (2, block);
        for (int ch = 0; ch < 2; ++ch)
            slice.copyFrom (ch, 0, wet, ch, off, block);
        ns.process (slice);
        for (int ch = 0; ch < 2; ++ch)
            wet.copyFrom (ch, off, slice, ch, 0, block);
    }

    // Measure 1 s right after the noise arrived (t 2-3 s) and again at 5-6 s.
    const float in = dry.getRMSLevel (0, 48000 * 2, 48000);
    const float late = wet.getRMSLevel (0, 48000 * 5, 48000);

    INFO ("in " << juce::Decibels::gainToDecibels (in) << " late " << juce::Decibels::gainToDecibels (late));
    const float lateDb = juce::Decibels::gainToDecibels (late / juce::jmax (in, 1.0e-9f));
    REQUIRE (lateDb <= -4.0f);  // estimate engaged within ~3 s of the noise
}
