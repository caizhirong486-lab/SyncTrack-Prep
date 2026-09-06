// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/Presets.h"
#include <cmath>
#include <random>

namespace
{
constexpr double sr = 48000.0;

/** Speech-like tone over a steady noise floor, so presets have something to
    differentiate on: level in the quiet part, noise in the steady part. */
juce::AudioBuffer<float> makeTestSignal()
{
    juce::AudioBuffer<float> buf (2, (int) sr * 6);
    std::mt19937 rng (1234);
    std::normal_distribution<float> noise (0.0f, 0.01f);

    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const float t = (float) i / (float) sr;
        const float amp = t < 3.0f ? 0.03f : 0.2f;
        const float voice = amp * std::sin (2.0f * juce::MathConstants<float>::pi * 320.0f * t)
                          * (0.6f + 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi * 3.0f * t));
        const float n = noise (rng);
        buf.setSample (0, i, voice + n);
        buf.setSample (1, i, voice * 0.85f + n);
    }
    return buf;
}

juce::AudioBuffer<float> renderPreset (const juce::AudioBuffer<float>& input, int preset, DenoiseMode mode, int block = 512)
{
    juce::AudioBuffer<float> buf (input);
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) block, (juce::uint32) buf.getNumChannels() };

    ChannelRepair cr;
    NoiseSuppressor ns;
    Leveler lv;
    PeakCompressor pc;
    TruePeakLimiter tp;
    cr.prepare (spec); ns.prepare (spec); lv.prepare (spec); pc.prepare (spec); tp.prepare (spec);

    const auto chain = Presets::chainFor (preset, mode);
    cr.setParams (chain.channelRepair);
    ns.setParams (chain.noiseSuppressor);
    lv.setParams (chain.leveler);
    pc.setParams (chain.peakCompressor);
    tp.setParams (chain.truePeakLimiter);

    for (int off = 0; off < buf.getNumSamples(); off += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - off);
        juce::AudioBuffer<float> slice (buf.getNumChannels(), n);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, buf, ch, off, n);

        cr.process (slice);
        lv.process (slice);
        ns.process (slice);
        pc.process (slice);
        tp.process (slice);

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.copyFrom (ch, off, slice, ch, 0, n);
    }
    return buf;
}

bool bitIdentical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return false;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            if (a.getSample (ch, i) != b.getSample (ch, i))
                return false;
    return true;
}
}

TEST_CASE ("Presets produce distinguishable output", "[presets]")
{
    // metric F: Soft / Strong / Clean must not collapse into the
    // same render. test5 shipped three "presets" that were bit-identical.
    const auto input = makeTestSignal();

    const auto soft = renderPreset (input, Presets::soft, Presets::denoiseModeDefault (Presets::soft));
    const auto strong = renderPreset (input, Presets::strong, Presets::denoiseModeDefault (Presets::strong));
    const auto clean = renderPreset (input, Presets::clean, Presets::denoiseModeDefault (Presets::clean));

    REQUIRE_FALSE (bitIdentical (soft, strong));
    REQUIRE_FALSE (bitIdentical (soft, clean));
    REQUIRE_FALSE (bitIdentical (strong, clean));
}

TEST_CASE ("Clean defaults to denoise on, Soft and Strong off", "[presets]")
{
    REQUIRE_FALSE (Presets::denoiseDefault (Presets::soft));
    REQUIRE_FALSE (Presets::denoiseDefault (Presets::strong));
    REQUIRE (Presets::denoiseDefault (Presets::clean));
}

TEST_CASE ("Strong lifts quiet material more than Clean", "[presets]")
{
    // Encodes the design intent of the table: Strong chases scene loudness,
    // Clean stays light so it does not amplify the residual noise floor.
    const auto input = makeTestSignal();
    const auto strong = renderPreset (input, Presets::strong, DenoiseMode::off);
    const auto clean = renderPreset (input, Presets::clean, DenoiseMode::off);

    auto rmsOf = [] (const juce::AudioBuffer<float>& b, int start, int len)
    {
        double s = 0.0;
        for (int i = start; i < start + len; ++i)
            s += (double) b.getSample (0, i) * b.getSample (0, i);
        return std::sqrt (s / (double) len);
    };

    const int quietStart = (int) sr * 1;
    const int quietLen = (int) sr * 2;
    const float strongDb = juce::Decibels::gainToDecibels ((float) rmsOf (strong, quietStart, quietLen), -100.0f);
    const float cleanDb = juce::Decibels::gainToDecibels ((float) rmsOf (clean, quietStart, quietLen), -100.0f);

    INFO ("strong " << strongDb << " dB vs clean " << cleanDb << " dB");
    REQUIRE (strongDb > cleanDb + 1.0f);
}

TEST_CASE ("Denoise amount defaults and warm-dynamics compressor constants", "[presets]")
{
    REQUIRE (Presets::denoiseAmountDefault (Presets::soft) == 40);
    REQUIRE (Presets::denoiseAmountDefault (Presets::strong) == 45);
    REQUIRE (Presets::denoiseAmountDefault (Presets::clean) == 55);

    REQUIRE (Presets::denoiseModeDefault (Presets::soft) == DenoiseMode::off);
    REQUIRE (Presets::denoiseModeDefault (Presets::strong) == DenoiseMode::live);
    REQUIRE (Presets::denoiseModeDefault (Presets::clean) == DenoiseMode::classic);

    for (int i = 0; i < Presets::count; ++i)
    {
        const auto chain = Presets::chainFor (i, Presets::denoiseModeDefault (i));
        REQUIRE (chain.peakCompressor.ratio == 2.0f);
        REQUIRE (chain.peakCompressor.attackMs == 15.0f);
        REQUIRE (chain.peakCompressor.releaseMs == 120.0f);
        REQUIRE (chain.peakCompressor.sceneOffsetDb == -4.0f);
        REQUIRE (chain.toneShaper.enabled);
        REQUIRE (chain.toneShaper.tone == 0.0f);
        REQUIRE (std::abs (chain.noiseSuppressor.amount
                           - (float) Presets::denoiseAmountDefault (i) / 100.0f) < 1.0e-4f);
        REQUIRE (chain.upwardExpander.enabled);
        REQUIRE (chain.upwardExpander.ratio == 1.5f);
        REQUIRE (chain.upwardExpander.rangeDb == 8.0f);
        REQUIRE (chain.upwardExpander.sceneOffsetDb == -20.0f);
    }

    REQUIRE (Presets::chainFor (Presets::strong, DenoiseMode::classic)
                 .noiseSuppressor.enabled);
    REQUIRE (! Presets::chainFor (Presets::strong, DenoiseMode::live)
                  .noiseSuppressor.enabled);
}
