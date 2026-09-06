// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "TestTruePeak.h"
#include "dsp/Presets.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <cstdlib>

namespace
{
juce::AudioBuffer<float> loadWav (const juce::File& f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, true);
    return buf;
}

float rmsDb (const juce::AudioBuffer<float>& b, int start, int len)
{
    double s = 0.0;
    const int nch = b.getNumChannels();
    int cnt = 0;
    for (int i = start; i < start + len; ++i)
    {
        for (int ch = 0; ch < nch; ++ch)
        {
            const float x = b.getSample (ch, i);
            s += (double) x * x;
            ++cnt;
        }
    }
    const float rms = (float) std::sqrt (s / (double) juce::jmax (1, cnt));
    return juce::Decibels::gainToDecibels (rms, -100.0f);
}

void processChain (juce::AudioBuffer<float>& buf, double sr)
{
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, buf.getNumSamples()), (juce::uint32) buf.getNumChannels() };

    ChannelRepair cr;
    NoiseSuppressor ns;
    Leveler lv;
    PeakCompressor pc;
    TruePeakLimiter tp;
    cr.prepare (spec); ns.prepare (spec); lv.prepare (spec); pc.prepare (spec); tp.prepare (spec);

    // Strong preset, straight from the shared table the plugin uses.
    const auto chain = Presets::chainFor (Presets::strong, DenoiseMode::off);
    cr.setParams (chain.channelRepair);
    ns.setParams (chain.noiseSuppressor);
    lv.setParams (chain.leveler);
    pc.setParams (chain.peakCompressor);
    tp.setParams (chain.truePeakLimiter);

    const int block = 512;
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
}
}

TEST_CASE ("Offline gold segment: soft region should be boosted", "[offline][.slow]")
{
    // Point STP_GOLD_WAV at a stereo reference clip to run this integration test
    const char* goldPath = std::getenv ("STP_GOLD_WAV");
    juce::File gold (goldPath != nullptr ? goldPath : "/tmp/reference.wav");
    if (! gold.existsAsFile())
    {
        WARN ("no reference wav found (set STP_GOLD_WAV) — skip offline integration");
        return;
    }

    auto buf = loadWav (gold);
    REQUIRE (buf.getNumSamples() > 48000 * 10);

    // Soft-ish region around second 20 (L-only dialogue zone in analysis)
    const int softStart = 20 * 48000;
    const int softLen = 3 * 48000;
    const float softIn = rmsDb (buf, softStart, softLen);

    // Louder region around second 12
    const int loudStart = 12 * 48000;
    const int loudLen = 1 * 48000;
    const float loudIn = rmsDb (buf, loudStart, loudLen);

    processChain (buf, 48000.0);

    const float softOut = rmsDb (buf, softStart, softLen);
    const float loudOut = rmsDb (buf, loudStart, loudLen);

    // Soft should rise noticeably; loud should not rise
    REQUIRE (softOut - softIn > 3.0f);
    REQUIRE (loudOut - loudIn < 1.0f);

    // Right channel should have energy in former L-only area
    float rEnergy = 0.0f;
    float maxLRDiff = 0.0f;
    for (int i = softStart; i < softStart + softLen; ++i)
    {
        rEnergy += buf.getSample (1, i) * buf.getSample (1, i);
        maxLRDiff = juce::jmax (maxLRDiff, std::abs (buf.getSample (0, i) - buf.getSample (1, i)));
    }
    REQUIRE (rEnergy > 1.0e-4f);
    // forceMono → L and R identical
    REQUIRE (maxLRDiff < 1.0e-5f);

    // metric E on real material. Measured on the loud region only:
    // the judge is an 8x reconstruction and is too slow for the whole file.
    const float tp = measureTruePeakDb (
        juce::AudioBuffer<float> (buf.getArrayOfWritePointers(), buf.getNumChannels(), loudStart, 3 * 48000),
        48000.0);
    INFO ("gold loud-region true peak " << tp << " dBTP");
    REQUIRE (tp <= -1.0f + 0.1f);
}
