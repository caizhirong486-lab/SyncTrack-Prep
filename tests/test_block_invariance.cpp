// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/Presets.h"
#include "dsp/ClassicDenoise.h"
#include <cmath>
#include <random>
#include <cstdlib>

namespace
{
constexpr double sr = 48000.0;

juce::AudioBuffer<float> makeSignal (int numSamples)
{
    juce::AudioBuffer<float> buf (2, numSamples);
    std::mt19937 rng (99);
    std::normal_distribution<float> noise (0.0f, 0.008f);
    for (int i = 0; i < numSamples; ++i)
    {
        const float t = (float) i / (float) sr;
        const float voice = 0.12f * std::sin (2.0f * juce::MathConstants<float>::pi * 280.0f * t)
                          + 0.05f * std::sin (2.0f * juce::MathConstants<float>::pi * 1900.0f * t);
        const float n = noise (rng);
        buf.setSample (0, i, voice + n);
        buf.setSample (1, i, voice * 0.9f + n);
    }
    return buf;
}

/** Runs one chain over the whole buffer using a fixed block size. Modules keep
    state across blocks, so the block size must not change the result. */
juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& input, int block, int preset, DenoiseMode mode, float tone = 0.0f)
{
    juce::AudioBuffer<float> buf (input);
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) block, (juce::uint32) buf.getNumChannels() };

    ChannelRepair cr;
    ClassicDenoise denoiseStage;
    Leveler lv;
    PeakCompressor pc;
    ToneShaper ts;
    UpwardExpander ue;
    TruePeakLimiter tp;
    cr.prepare (spec); denoiseStage.prepare (sr, 512, 2); lv.prepare (spec); pc.prepare (spec);
    ts.prepare (spec); ue.prepare (spec); tp.prepare (spec);

    auto chain = Presets::chainFor (preset, mode);
    chain.toneShaper.tone = tone;
    cr.setParams (chain.channelRepair);
    denoiseStage.setClassicParams (chain.noiseSuppressor);
    lv.setParams (chain.leveler);
    pc.setParams (chain.peakCompressor);
    ts.setParams (chain.toneShaper);
    ue.setParams (chain.upwardExpander);
    tp.setParams (chain.truePeakLimiter);
    // Continuous per-sample scene level, no output gain offline.
    pc.bindSceneSource (&lv.sceneStream());
    ue.bindSceneSource (&lv.sceneStream());

    for (int off = 0; off < buf.getNumSamples(); off += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - off);
        juce::AudioBuffer<float> slice (buf.getNumChannels(), n);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, buf, ch, off, n);

        cr.process (slice);
        lv.process (slice);
        denoiseStage.process (slice);
        if (std::getenv ("STP_SKIP_TS") == nullptr) ts.process (slice);
        ue.setSceneLevelDb (0.0f);
        if (std::getenv ("STP_SKIP_UE") == nullptr) ue.process (slice);
        pc.setSceneLevelDb (0.0f);
        if (std::getenv ("STP_SKIP_PC") == nullptr) pc.process (slice);
        if (std::getenv ("STP_SKIP_TP") == nullptr) tp.process (slice);

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.copyFrom (ch, off, slice, ch, 0, n);
    }
    return buf;
}

float maxAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    REQUIRE (a.getNumChannels() == b.getNumChannels());
    REQUIRE (a.getNumSamples() == b.getNumSamples());
    float d = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            d = juce::jmax (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
    return d;
}
}

TEST_CASE ("Chain output is independent of host block size", "[blocksize]")
{
    // Hosts pick their own buffer size, and STFT/FIFO stages are exactly where
    // that leaks into the audio. Without this, a DAW-only difference in output
    // has no test that can catch it.
    const auto input = makeSignal ((int) sr * 4);

    for (DenoiseMode mode : { DenoiseMode::off, DenoiseMode::classic })
    {
        const auto reference = render (input, 512, Presets::strong, mode);
        for (int block : { 32, 64, 128, 480, 1024, 2048 })
        {
            const auto other = render (input, block, Presets::strong, mode);
            const float diff = maxAbsDiff (reference, other);
            INFO ("mode " << (int) mode << ", block " << block << ", max diff " << diff);
            REQUIRE (diff < 1.0e-6f);
        }
    }
}

TEST_CASE ("Denoise STFT satisfies COLA on a near-transparent setting", "[denoise][cola]")
{
    // test1 shipped a broken hop/OLA that turned every hop into a
    // discontinuity. With overlap-add correct and the spectral gain pinned near
    // unity, the STFT path must reconstruct the input, only delayed.
    NoiseSuppressor ns;
    const int block = 512;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) block, 2 };
    ns.prepare (spec);

    NoiseSuppressor::Params p;
    p.enabled = true;
    p.amount = 0.02f;        // above the bypass threshold, so the STFT runs
    p.hpfHz = 1.0f;          // push the HPF phase shift out of the test band
    p.overSubtract = 0.0f;   // no subtraction
    p.speechProtect = 1.0f;  // hold the gain at unity
    p.noiseFloorDb = -120.0f;
    ns.setParams (p);

    const int n = (int) sr * 2;
    auto input = makeSignal (n);
    juce::AudioBuffer<float> out (input);

    for (int off = 0; off < n; off += block)
    {
        const int len = juce::jmin (block, n - off);
        juce::AudioBuffer<float> slice (2, len);
        for (int ch = 0; ch < 2; ++ch)
            slice.copyFrom (ch, 0, out, ch, off, len);
        ns.process (slice);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, off, slice, ch, 0, len);
    }

    // Compare against the input delayed by the reported latency, skipping the
    // priming region and the HPF's settling time.
    const int latency = ns.getLatencySamples();
    REQUIRE (latency > 0);

    const int start = latency + 4096;
    const int end = n - 4096;
    double err = 0.0, ref = 0.0;
    float worst = 0.0f;
    for (int i = start; i < end; ++i)
    {
        const float a = input.getSample (0, i - latency);
        const float b = out.getSample (0, i);
        err += (double) (b - a) * (b - a);
        ref += (double) a * a;
        worst = juce::jmax (worst, std::abs (b - a));
    }

    const float errDb = juce::Decibels::gainToDecibels ((float) std::sqrt (err / juce::jmax (1.0, ref)), -200.0f);
    INFO ("reconstruction error " << errDb << " dB, worst sample " << worst);
    // A hop discontinuity shows up as broadband error well above this.
    REQUIRE (errDb < -40.0f);
}

TEST_CASE ("Denoise disabled is a pure delay, unchanged audio", "[denoise]")
{
    NoiseSuppressor ns;
    const int block = 256;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) block, 2 };
    ns.prepare (spec);

    NoiseSuppressor::Params p;
    p.enabled = false;
    ns.setParams (p);

    const int n = (int) sr;
    const auto input = makeSignal (n);
    juce::AudioBuffer<float> out (input);

    for (int off = 0; off < n; off += block)
    {
        const int len = juce::jmin (block, n - off);
        juce::AudioBuffer<float> slice (2, len);
        for (int ch = 0; ch < 2; ++ch)
            slice.copyFrom (ch, 0, out, ch, off, len);
        ns.process (slice);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, off, slice, ch, 0, len);
    }

    // The disabled path keeps the STFT delay so the reported latency stays
    // constant; the audio itself must pass through unchanged.
    const int latency = ns.getLatencySamples();
    REQUIRE (latency > 0);
    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = latency; i < n; ++i)
            worst = juce::jmax (worst, std::abs (out.getSample (ch, i) - input.getSample (ch, i - latency)));
    REQUIRE (worst == 0.0f);
}

TEST_CASE ("ToneShaped chain is independent of host block size", "[blocksize][tone]")
{
    // The ToneShaper's biquad state must carry across blocks exactly like the
    // other stages, including a live tone setting (not the transparent zero).
    const auto input = makeSignal ((int) sr * 4);

    const auto reference = render (input, 512, Presets::strong, DenoiseMode::off, 0.8f);
    for (int block : { 64, 333, 1024 })
    {
        const auto other = render (input, block, Presets::strong, DenoiseMode::off, 0.8f);
        const float diff = maxAbsDiff (reference, other);
        INFO ("block " << block << ", max diff " << diff);
        REQUIRE (diff < 1.0e-6f);
    }
}
