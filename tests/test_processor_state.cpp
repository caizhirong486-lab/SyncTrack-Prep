// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "PluginProcessor.h"
#include "TestTruePeak.h"

namespace
{
void setBool (juce::AudioProcessorValueTreeState& apvts, const char* id, bool on)
{
    auto* p = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (id));
    REQUIRE (p != nullptr);
    p->setValueNotifyingHost (on ? 1.0f : 0.0f);
}

void setChoice (juce::AudioProcessorValueTreeState& apvts, const char* id, int index)
{
    auto* p = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id));
    REQUIRE (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
}

bool getBool (juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    auto* p = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (id));
    REQUIRE (p != nullptr);
    return p->get();
}

int getChoice (juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    auto* p = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id));
    REQUIRE (p != nullptr);
    return p->getIndex();
}

float getFloat (juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    auto* p = apvts.getParameter (id);
    REQUIRE (p != nullptr);
    return p->convertFrom0to1 (p->getValue());
}
}

TEST_CASE ("State round-trip keeps a manual denoise choice", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Soft defaults denoise off; the user turns it on by hand.
    juce::MemoryBlock saved;
    {
        SyncTrackPrepProcessor src;
        setChoice (src.apvts, "preset", 0);
        setBool (src.apvts, "denoise", true);
        src.getStateInformation (saved);
        REQUIRE (getChoice (src.apvts, "preset") == 0);
        REQUIRE (getBool (src.apvts, "denoise"));
    }

    SyncTrackPrepProcessor dst;
    dst.setStateInformation (saved.getData(), (int) saved.getSize());

    REQUIRE (getChoice (dst.apvts, "preset") == 0);
    // Would fail if the restored preset re-applied its default of denoise off.
    REQUIRE (getBool (dst.apvts, "denoise"));
}

TEST_CASE ("State round-trip keeps denoise off under Clean", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Clean defaults denoise on; the user turns it off by hand. This is the
    // direction that survives even without the guard only by child ordering.
    juce::MemoryBlock saved;
    {
        SyncTrackPrepProcessor src;
        setChoice (src.apvts, "preset", 2);
        REQUIRE (getBool (src.apvts, "denoise")); // preset default applied
        setBool (src.apvts, "denoise", false);
        src.getStateInformation (saved);
    }

    SyncTrackPrepProcessor dst;
    dst.setStateInformation (saved.getData(), (int) saved.getSize());

    REQUIRE (getChoice (dst.apvts, "preset") == 2);
    REQUIRE (! getBool (dst.apvts, "denoise"));
}

TEST_CASE ("State round-trip keeps output gain and bypass", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::MemoryBlock saved;
    {
        SyncTrackPrepProcessor src;
        setChoice (src.apvts, "preset", 1);
        setBool (src.apvts, "bypass", true);
        auto* gain = src.apvts.getParameter ("outputGain");
        REQUIRE (gain != nullptr);
        gain->setValueNotifyingHost (gain->convertTo0to1 (-6.0f));
        src.getStateInformation (saved);
    }

    SyncTrackPrepProcessor dst;
    dst.setStateInformation (saved.getData(), (int) saved.getSize());

    REQUIRE (getChoice (dst.apvts, "preset") == 1);
    REQUIRE (getBool (dst.apvts, "bypass"));
    REQUIRE (std::abs (getFloat (dst.apvts, "outputGain") - (-6.0f)) < 0.05f);
}

TEST_CASE ("Switching preset still applies its denoise default", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SyncTrackPrepProcessor p;
    setChoice (p.apvts, "preset", 0);
    REQUIRE (! getBool (p.apvts, "denoise"));

    setChoice (p.apvts, "preset", 2); // Clean
    REQUIRE (getBool (p.apvts, "denoise"));

    setChoice (p.apvts, "preset", 1); // Strong
    REQUIRE (! getBool (p.apvts, "denoise"));
}


TEST_CASE ("Reported latency is fixed while Denoise toggles", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SyncTrackPrepProcessor p;
    p.prepareToPlay (48000.0, 512);

    const int latOff = p.getLatencySamples();
    setBool (p.apvts, "denoise", true);
    const int latOn = p.getLatencySamples();
    setBool (p.apvts, "denoise", false);
    const int latOffAgain = p.getLatencySamples();

    INFO ("latency off " << latOff << " / on " << latOn << " / off " << latOffAgain);
    REQUIRE (latOn == latOff);
    REQUIRE (latOffAgain == latOff);
    // Denoise STFT delay (511) + limiter look-ahead (64).
    REQUIRE (latOff == NoiseSuppressor::latencyWhenEnabled + TruePeakLimiter::lookaheadSamples);
}

namespace
{
juce::AudioBuffer<float> runProcessor (SyncTrackPrepProcessor& p, const juce::AudioBuffer<float>& input, int block = 512)
{
    juce::AudioBuffer<float> out (input);
    juce::MidiBuffer midi;
    for (int off = 0; off < out.getNumSamples(); off += block)
    {
        const int n = juce::jmin (block, out.getNumSamples() - off);
        juce::AudioBuffer<float> slice (2, n);
        for (int ch = 0; ch < 2; ++ch)
            slice.copyFrom (ch, 0, out, ch, off, n);
        p.processBlock (slice, midi);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, off, slice, ch, 0, n);
    }
    return out;
}

float rmsDbOf (const juce::AudioBuffer<float>& b, int start, int len)
{
    double s = 0.0;
    for (int i = start; i < start + len; ++i)
    {
        const float x = b.getSample (0, i);
        s += (double) x * x;
    }
    return juce::Decibels::gainToDecibels ((float) std::sqrt (s / (double) len), -120.0f);
}
}

TEST_CASE ("Output +12 dB is clamped by the safety chain", "[state][truepeak]")
{
    // Output gain sits ahead of comp+limiter, so the -1 dBTP promise
    // holds at any knob position and the knob adds almost no loudness above
    // the ceiling. Locks that behaviour so a future reordering cannot silently
    // reintroduce that breach.
    juce::ScopedJuceInitialiser_GUI juceInit;

    const int n = 48000 * 2;
    const int fade = 600;
    juce::AudioBuffer<float> input (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / 48000.0f;
        const float amp = t < 1.0f ? 0.02f : 0.9f;
        // Faded edges: an abrupt burst is a step discontinuity whose own
        // inter-sample overshoot reads +0.5 dB in any interpolator, which
        // would swamp the ceiling check (see TestTruePeak.h).
        float w = 1.0f;
        if (i >= 48000 - fade && i < 48000)
            w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - (48000 - fade)) / (float) fade);
        else if (i >= 48000 && i < 48000 + fade)
            w = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - 48000) / (float) fade);
        else if (i >= n - 1176) // fade ends at n-576 so the 575-sample chain delay leaves the tail at zero
            w = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - (n - 1176)) / (float) fade);
        const float s = amp * w * std::sin (2.0f * juce::MathConstants<float>::pi * 9000.0f * t);
        input.setSample (0, i, s);
        input.setSample (1, i, s * 0.8f);
    }

    SyncTrackPrepProcessor p;
    p.prepareToPlay (48000.0, 512);
    setChoice (p.apvts, "preset", 1); // Strong, denoise off

    auto* gain = p.apvts.getParameter ("outputGain");
    REQUIRE (gain != nullptr);

    gain->setValueNotifyingHost (gain->convertTo0to1 (0.0f));
    const auto out0 = runProcessor (p, input);

    // A fresh instance for the +12 dB pass: AudioProcessor::reset() does not
    // reset the custom DSP modules, so reusing `p` would inherit the limiter's
    // pressed-down gain and the leveler's settled envelope from pass one.
    SyncTrackPrepProcessor p2;
    p2.prepareToPlay (48000.0, 512);
    setChoice (p2.apvts, "preset", 1);
    auto* gain2 = p2.apvts.getParameter ("outputGain");
    REQUIRE (gain2 != nullptr);
    gain2->setValueNotifyingHost (gain2->convertTo0to1 (12.0f));
    const auto out12 = runProcessor (p2, input);

    const float tp = measureTruePeakDb (out12, 48000.0);
    INFO ("output +12 dB true peak " << tp << " dBTP");
    REQUIRE (tp <= -1.0f + 0.15f);

    // The ceiling only constrains content that actually hits it. The leveler
    // pulls steady content down to its target before the knob, so +12 dB of
    // makeup lifts that content until it meets the ceiling - the knob is a
    // real makeup gain, and the safety chain clamps whatever exceeds the
    // ceiling. The promise that must hold everywhere is the true peak, which
    // is asserted above; the quiet section should show the makeup passing
    // through.
    const float r0quiet = rmsDbOf (out0, 24000, 12000);
    const float r12quiet = rmsDbOf (out12, 24000, 12000);
    INFO ("quiet-section rms at 0 dB " << r0quiet << " vs +12 dB " << r12quiet);
    REQUIRE (r12quiet - r0quiet >= 10.0f); // makeup gain passes through

    // Latency must be untouched by the reorder.
    REQUIRE (p.getLatencySamples() == NoiseSuppressor::latencyWhenEnabled + TruePeakLimiter::lookaheadSamples);
}

TEST_CASE ("Re-selecting the same preset keeps the manual denoise choice", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SyncTrackPrepProcessor p;
    setChoice (p.apvts, "preset", 2); // Clean -> denoise on by default
    REQUIRE (getBool (p.apvts, "denoise"));

    setBool (p.apvts, "denoise", false); // user turns it off

    // Clicking the same preset again is not a gesture and must not reset it.
    setChoice (p.apvts, "preset", 2);
    REQUIRE (! getBool (p.apvts, "denoise"));

    // A real switch to another preset still applies its default.
    setChoice (p.apvts, "preset", 0); // Soft -> denoise off (already off)
    REQUIRE (! getBool (p.apvts, "denoise"));
    setBool (p.apvts, "denoise", true); // user turns it on
    setChoice (p.apvts, "preset", 2);  // Clean -> denoise on (matches, no reset)
    REQUIRE (getBool (p.apvts, "denoise"));
    setBool (p.apvts, "denoise", false);
    setChoice (p.apvts, "preset", 2); // same preset again -> keeps off
    REQUIRE (! getBool (p.apvts, "denoise"));
}
