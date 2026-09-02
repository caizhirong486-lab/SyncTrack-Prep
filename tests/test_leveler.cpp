// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/Leveler.h"
#include <cmath>
#include <random>

static float bufferRms (const juce::AudioBuffer<float>& b, int start, int len)
{
    double s = 0.0;
    for (int i = start; i < start + len; ++i)
    {
        const float x = b.getSample (0, i);
        s += (double) x * (double) x;
    }
    return (float) std::sqrt (s / (double) juce::jmax (1, len));
}

/** Syllabic envelope: 3 Hz, duty ~70%, dips to 20% of the peak. Dialogue-like
    amplitude modulation is what the noise gate discriminates on. */
static float syllableEnv (double t)
{
    const float e = 0.5f - 0.5f * std::cos (2.0 * juce::MathConstants<double>::pi * 3.0 * t);
    return 0.2f + 0.8f * e;
}

TEST_CASE ("Leveler boosts soft active content", "[leveler]")
{
    Leveler lv;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    lv.prepare (spec);

    Leveler::Params p;
    p.enabled = true;
    p.targetDb = -18.0f;
    p.strength = 1.0f;
    p.maxGainDb = 20.0f;
    p.maxAttenDb = 20.0f;
    p.gateDb = -55.0f;
    p.slowTauSec = 1.0f;
    p.fastTauSec = 0.03f;
    p.attackSec = 0.05f;
    p.releaseSec = 0.3f;
    lv.setParams (p);

    // Modulated (speech-like) content, not a steady tone: steady tones are
    // interference under the noise gate and are deliberately not boosted.
    const float amp = juce::Decibels::decibelsToGain (-40.0f);
    juce::AudioBuffer<float> buf (1, 96000);
    for (int i = 0; i < 96000; ++i)
    {
        const float t = (float) i / 48000.0f;
        buf.setSample (0, i, amp * syllableEnv (t)
                       * std::sin (2.0f * juce::MathConstants<float>::pi * 500.0f * t));
    }

    lv.process (buf);
    const float outRms = bufferRms (buf, 72000, 12000);
    const float outDb = juce::Decibels::gainToDecibels (outRms, -80.0f);
    REQUIRE (outDb > -30.0f);
}

TEST_CASE ("Leveler does not boost pure silence", "[leveler]")
{
    Leveler lv;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    lv.prepare (spec);
    Leveler::Params p;
    p.strength = 1.0f;
    p.maxGainDb = 24.0f;
    p.gateDb = -42.0f;
    lv.setParams (p);

    juce::AudioBuffer<float> buf (1, 8192);
    buf.clear();
    lv.process (buf);
    REQUIRE (buf.getMagnitude (0, 0, 8192) < 1.0e-6f);
}

TEST_CASE ("Dual-time leveler reduces long-term segment gap", "[leveler]")
{
    Leveler lv;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    lv.prepare (spec);
    Leveler::Params p;
    p.strength = 0.85f;
    p.targetDb = -20.0f;
    p.maxGainDb = 18.0f;
    p.maxAttenDb = 12.0f;
    p.gateDb = -50.0f;
    p.slowTauSec = 1.5f;
    p.fastTauSec = 0.03f;
    p.releaseSec = 0.4f;
    p.attackSec = 0.1f;
    lv.setParams (p);

    // 5s loud then 5s soft (slow envelope needs time), both syllable-modulated
    juce::AudioBuffer<float> buf (1, 48000 * 10);
    for (int i = 0; i < 48000 * 5; ++i)
    {
        const float t = (float) i / 48000.0f;
        buf.setSample (0, i, 0.2f * syllableEnv (t) * std::sin (0.1f * (float) i));
    }
    for (int i = 48000 * 5; i < 48000 * 10; ++i)
    {
        const float t = (float) i / 48000.0f;
        buf.setSample (0, i, 0.02f * syllableEnv (t) * std::sin (0.1f * (float) i));
    }

    const float loudIn = bufferRms (buf, 48000, 48000 * 2);
    const float softIn = bufferRms (buf, 48000 * 7, 48000 * 2);
    const float gapIn = std::abs (juce::Decibels::gainToDecibels (loudIn) - juce::Decibels::gainToDecibels (softIn));

    lv.process (buf);

    const float loudOut = bufferRms (buf, 48000 * 2, 48000 * 2);
    const float softOut = bufferRms (buf, 48000 * 8, 48000 * 2);
    const float gapOut = std::abs (juce::Decibels::gainToDecibels (loudOut) - juce::Decibels::gainToDecibels (softOut));

    REQUIRE (gapOut < gapIn * 0.75f);
}

TEST_CASE ("Leveler releases gain quickly after loud then silence", "[leveler]")
{
    Leveler lv;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    lv.prepare (spec);
    Leveler::Params p;
    p.strength = 1.0f;
    p.targetDb = -18.0f;
    p.maxGainDb = 18.0f;
    p.gateDb = -42.0f;
    lv.setParams (p);

    juce::AudioBuffer<float> buf (1, 48000 * 3);
    const float amp = juce::Decibels::decibelsToGain (-36.0f);
    for (int i = 0; i < 48000; ++i)
    {
        const float t = (float) i / 48000.0f;
        buf.setSample (0, i, amp * syllableEnv (t)
                       * std::sin (2.0f * juce::MathConstants<float>::pi * 400.0f * t));
    }
    for (int i = 48000; i < 48000 * 3; ++i)
        buf.setSample (0, i, 0.0f);

    lv.process (buf);
    const float tailPeak = buf.getMagnitude (0, 48000 + 24000, 24000);
    REQUIRE (tailPeak < 1.0e-4f);
}

TEST_CASE ("Leveler does not lift a steady noise floor", "[leveler][noise-gate]")
{
    // Metric C root cause: a -31 dB steady floor sits above the level gate and
    // used to be chased to the target by the full max-gain. The noise gate
    // must hold it at unity.
    Leveler lv;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    lv.prepare (spec);
    Leveler::Params p;
    p.strength = 0.75f; // Strong
    p.targetDb = -18.0f;
    p.maxGainDb = 18.0f;
    p.maxAttenDb = 12.0f;
    p.gateDb = -48.0f;
    p.slowTauSec = 2.5f;
    lv.setParams (p);

    const float amp = juce::Decibels::decibelsToGain (-31.0f);
    juce::AudioBuffer<float> buf (1, 48000 * 6);
    std::mt19937 rng (11);
    std::normal_distribution<float> noise (0.0f, amp);
    for (int i = 0; i < buf.getNumSamples(); ++i)
        buf.setSample (0, i, noise (rng));

    const float inRms = bufferRms (buf, 48000 * 3, 48000 * 3);
    lv.process (buf);
    const float outRms = bufferRms (buf, 48000 * 3, 48000 * 3);
    const float liftDb = juce::Decibels::gainToDecibels (outRms / juce::jmax (inRms, 1.0e-9f), -100.0f);

    INFO ("steady-floor lift " << liftDb << " dB (was +12.9 dB on real material before the gate)");
    REQUIRE (liftDb < 1.0f);
}

TEST_CASE ("Leveler still lifts whisper-like modulated noise", "[leveler][noise-gate]")
{
    // Whisper is amplitude-modulated broadband noise: no periodicity, but
    // strong syllabic envelope. It must stay on the dialogue side of the gate
    // (a ZCR-based detector would classify it as noise and drop it).
    Leveler lv;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    lv.prepare (spec);
    Leveler::Params p;
    p.strength = 0.75f;
    p.targetDb = -18.0f;
    p.maxGainDb = 18.0f;
    p.maxAttenDb = 12.0f;
    p.gateDb = -48.0f;
    p.slowTauSec = 2.5f;
    lv.setParams (p);

    const float amp = juce::Decibels::decibelsToGain (-38.0f);
    juce::AudioBuffer<float> buf (1, 48000 * 5);
    std::mt19937 rng (13);
    std::normal_distribution<float> noise (0.0f, amp);
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const float t = (float) i / 48000.0f;
        buf.setSample (0, i, noise (rng) * syllableEnv (t));
    }

    const float inRms = bufferRms (buf, 48000 * 2, 48000 * 3);
    lv.process (buf);
    const float outRms = bufferRms (buf, 48000 * 2, 48000 * 3);
    const float liftDb = juce::Decibels::gainToDecibels (outRms / juce::jmax (inRms, 1.0e-9f), -100.0f);

    INFO ("whisper lift " << liftDb << " dB");
    REQUIRE (liftDb > 4.0f);
}

TEST_CASE ("Noise gate hands over between dialogue and floor without pumping", "[leveler][noise-gate]")
{
    // 3 s modulated dialogue, 5 s steady floor, 3 s dialogue again. The floor
    // must come back to roughly unity (not chased) once the classifier has
    // settled (~1 s hand-over), the dialogue must be lifted again, and the
    // gain must not oscillate: the second dialogue block settles to a boost
    // close to the first one.
    Leveler lv;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    lv.prepare (spec);
    Leveler::Params p;
    p.strength = 0.75f;
    p.targetDb = -18.0f;
    p.maxGainDb = 18.0f;
    p.maxAttenDb = 12.0f;
    p.gateDb = -48.0f;
    p.slowTauSec = 2.5f;
    lv.setParams (p);

    const float dialAmp = juce::Decibels::decibelsToGain (-33.0f);
    const float floorAmp = juce::Decibels::decibelsToGain (-31.0f);
    const int n = 48000 * 11;
    juce::AudioBuffer<float> buf (1, n);
    std::mt19937 rng (17);
    std::normal_distribution<float> noise (0.0f, 1.0f);

    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / 48000.0f;
        if (i < 48000 * 3 || i >= 48000 * 8)
        {
            const float e = syllableEnv (t);
            buf.setSample (0, i, dialAmp * e * noise (rng));
        }
        else
        {
            buf.setSample (0, i, floorAmp * noise (rng));
        }
    }

    juce::AudioBuffer<float> dry (buf);
    lv.process (buf);

    const auto liftDb = [&] (int start, int len)
    {
        return juce::Decibels::gainToDecibels (
            bufferRms (buf, start, len) / juce::jmax (bufferRms (dry, start, len), 1.0e-9f), -100.0f);
    };

    const float floorEarly = liftDb (48000 * 4, 48000);      // hand-over swell, bounded
    const float floorLift = liftDb (48000 * 5, 48000 * 2);   // steady, classifier settled
    const float dial1 = liftDb (48000 * 1, 48000 * 2);      // first dialogue block
    const float dial2 = liftDb (48000 * 8 + 24000, 48000);   // second dialogue block, settled

    INFO ("floor lift " << floorLift << " dB (early " << floorEarly << " dB), dialogue lifts "
          << dial1 << " / " << dial2 << " dB");
    REQUIRE (floorLift < 1.5f);       // floor not chased once settled
    REQUIRE (floorEarly < 6.0f);       // hand-over swell is bounded, not the old +12.9 dB chase
    REQUIRE (dial1 > 4.0f);           // dialogue lifted
    REQUIRE (dial2 > 4.0f);           // gate re-opens after the floor
    // No pumping: the two dialogue lifts agree within 2 dB.
    REQUIRE (std::abs (dial1 - dial2) < 2.0f);
}

TEST_CASE ("Leveler never gates a sustained vowel", "[leveler][noise-gate]")
{
    // A long steady vowel has almost no envelope modulation (mod ~0.02,
    // below the enter threshold), but it is dialogue and must keep its boost.
    // The gate only engages for low-modulation AND low-level content; a
    // mid-level vowel must never lose its gain mid-phrase.
    Leveler lv;
    juce::dsp::ProcessSpec spec { 48000.0, 512, 1 };
    lv.prepare (spec);
    Leveler::Params p;
    p.strength = 0.75f;
    p.targetDb = -18.0f;
    p.maxGainDb = 18.0f;
    p.maxAttenDb = 12.0f;
    p.gateDb = -48.0f;
    p.slowTauSec = 2.5f;
    lv.setParams (p);

    const float amp = juce::Decibels::decibelsToGain (-20.0f);
    juce::AudioBuffer<float> buf (1, 48000 * 6);
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const float t = (float) i / 48000.0f;
        buf.setSample (0, i, amp * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * t));
    }

    const float inRms = bufferRms (buf, 48000 * 2, 48000 * 3);
    lv.process (buf);
    const float outRms = bufferRms (buf, 48000 * 2, 48000 * 3);
    const float liftDb = juce::Decibels::gainToDecibels (outRms / juce::jmax (inRms, 1.0e-9f), -100.0f);

    INFO ("sustained-vowel lift " << liftDb << " dB");
    REQUIRE (liftDb > 4.0f);
}
