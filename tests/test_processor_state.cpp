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

void setFloat (juce::AudioProcessorValueTreeState& apvts, const char* id, float value)
{
    auto* p = apvts.getParameter (id);
    REQUIRE (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (value));
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
    setChoice (p.apvts, "preset", 0); // Soft
    REQUIRE (getChoice (p.apvts, "denoiseMode") == (int) DenoiseMode::off);
    REQUIRE (! getBool (p.apvts, "denoise"));

    setChoice (p.apvts, "preset", 2); // Clean -> classic spectral denoiser
    REQUIRE (getChoice (p.apvts, "denoiseMode") == (int) DenoiseMode::classic);
    REQUIRE (getBool (p.apvts, "denoise"));

    setChoice (p.apvts, "preset", 1); // Strong -> NN Live engine
    REQUIRE (getChoice (p.apvts, "denoiseMode") == (int) DenoiseMode::live);
    REQUIRE (getBool (p.apvts, "denoise"));
}

TEST_CASE ("Mode round-trip and legacy-bool key precedence", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // A saved Classic session must stay Classic even though the mirrored
    // legacy bool reads "on" — the denoiseMode key takes precedence.
    juce::MemoryBlock saved;
    {
        SyncTrackPrepProcessor src;
        setChoice (src.apvts, "preset", 1);
        setChoice (src.apvts, "denoiseMode", (int) DenoiseMode::classic);
        src.getStateInformation (saved);
    }
    SyncTrackPrepProcessor dst;
    dst.setStateInformation (saved.getData(), (int) saved.getSize());
    REQUIRE (getChoice (dst.apvts, "denoiseMode") == (int) DenoiseMode::classic);

    // Strip denoiseMode -> true legacy state: bool on maps to Live (upgrade
    // mapping), bool off maps to Off.
    auto makeLegacy = [] (const juce::MemoryBlock& block, bool on) -> juce::MemoryBlock
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
        REQUIRE (xml != nullptr);
        bool removed = false;
        while (auto* modeChild = xml->getChildByAttribute ("id", juce::String ("denoiseMode")))
        {
            xml->removeChildElement (modeChild, true);
            removed = true;
        }
        REQUIRE (removed);
        if (auto* denoiseChild = xml->getChildByAttribute ("id", juce::String ("denoise")))
            denoiseChild->setAttribute ("value", on ? juce::String ("1") : juce::String ("0"));
        INFO ("denoise attr now: "
              << (xml->getChildByAttribute ("id", juce::String ("denoise"))
                      ? xml->getChildByAttribute ("id", juce::String ("denoise"))->getStringAttribute ("value")
                      : juce::String ("<missing>")));
        juce::MemoryBlock out;
        juce::AudioProcessor::copyXmlToBinary (*xml, out);
        auto reparsed = juce::AudioProcessor::getXmlFromBinary (out.getData(), (int) out.getSize());
        CHECK (reparsed != nullptr);
        CHECK (reparsed->getChildByAttribute ("id", juce::String ("denoiseMode")) == nullptr);
        return out;
    };

    auto legacyOn = makeLegacy (saved, true);
    SyncTrackPrepProcessor legacyLoader;
    legacyLoader.setStateInformation (legacyOn.getData(), (int) legacyOn.getSize());
    REQUIRE (getChoice (legacyLoader.apvts, "denoiseMode") == (int) DenoiseMode::live);
    REQUIRE (getBool (legacyLoader.apvts, "denoise"));

    auto legacyOff = makeLegacy (saved, false);
    SyncTrackPrepProcessor legacyOffLoader;
    legacyOffLoader.setStateInformation (legacyOff.getData(), (int) legacyOff.getSize());
    INFO ("after legacy-off load: mode idx " << getChoice (legacyOffLoader.apvts, "denoiseMode")
          << ", bool " << (int) getBool (legacyOffLoader.apvts, "denoise"));
    CHECK (getChoice (legacyOffLoader.apvts, "denoiseMode") == (int) DenoiseMode::off);
    CHECK (! getBool (legacyOffLoader.apvts, "denoise"));
}

TEST_CASE ("Output range: 0 dB sits at the normalised centre", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SyncTrackPrepProcessor p;
    auto* gain = p.apvts.getParameter ("outputGain");
    REQUIRE (gain != nullptr);
    REQUIRE (std::abs (gain->convertTo0to1 (0.0f) - 0.5f) < 1.0e-3f);
    REQUIRE (std::abs (gain->convertFrom0to1 (0.5f)) < 0.05f);
}


TEST_CASE ("Reported latency is fixed between Off and Classic", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SyncTrackPrepProcessor p;
    setChoice (p.apvts, "denoiseMode", (int) DenoiseMode::off);
    p.prepareToPlay (48000.0, 512);

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> probe (2, 512);
    auto runOneBlock = [&]
    {
        probe.clear();
        p.processBlock (probe, midi);
        p.flushPendingLatency();
    };

    runOneBlock();
    const int latOff = p.getLatencySamples();
    setChoice (p.apvts, "denoiseMode", (int) DenoiseMode::classic);
    runOneBlock();
    const int latClassic = p.getLatencySamples();
    setChoice (p.apvts, "denoiseMode", (int) DenoiseMode::off);
    runOneBlock();
    const int latOffAgain = p.getLatencySamples();

    INFO ("latency off " << latOff << " / classic " << latClassic << " / off " << latOffAgain);
    REQUIRE (latClassic == latOff);
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
    setChoice (p.apvts, "preset", 1);
    setChoice (p.apvts, "denoiseMode", (int) DenoiseMode::off); // dry safety chain

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
    setChoice (p2.apvts, "denoiseMode", (int) DenoiseMode::off);
    auto* gain2 = p2.apvts.getParameter ("outputGain");
    REQUIRE (gain2 != nullptr);
    gain2->setValueNotifyingHost (gain2->convertTo0to1 (12.0f));
    const auto out12 = runProcessor (p2, input);
    p.flushPendingLatency();

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

TEST_CASE ("State round-trip keeps amount and tone knobs", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::MemoryBlock saved;
    {
        SyncTrackPrepProcessor src;
        setChoice (src.apvts, "preset", 1);
        setFloat (src.apvts, "denoiseAmount", 72.0f);
        setFloat (src.apvts, "tone", -0.4f);
        src.getStateInformation (saved);
    }

    SyncTrackPrepProcessor dst;
    dst.setStateInformation (saved.getData(), (int) saved.getSize());

    REQUIRE (std::abs (getFloat (dst.apvts, "denoiseAmount") - 72.0f) < 0.05f);
    REQUIRE (std::abs (getFloat (dst.apvts, "tone") - (-0.4f)) < 0.005f);
}

TEST_CASE ("Switching preset applies its amount default", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SyncTrackPrepProcessor p;
    REQUIRE (std::abs (getFloat (p.apvts, "denoiseAmount") - 45.0f) < 0.05f); // Strong seed

    setChoice (p.apvts, "preset", 0); // Soft
    REQUIRE (std::abs (getFloat (p.apvts, "denoiseAmount") - 40.0f) < 0.05f);

    setChoice (p.apvts, "preset", 2); // Clean
    REQUIRE (std::abs (getFloat (p.apvts, "denoiseAmount") - 55.0f) < 0.05f);
}

TEST_CASE ("Re-selecting the same preset keeps a manual amount", "[state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SyncTrackPrepProcessor p;
    setChoice (p.apvts, "preset", 1);
    setFloat (p.apvts, "denoiseAmount", 80.0f); // user pushes the knob

    // Same preset again is not a gesture.
    setChoice (p.apvts, "preset", 1);
    REQUIRE (std::abs (getFloat (p.apvts, "denoiseAmount") - 80.0f) < 0.05f);

    // A real switch reseeds the default.
    setChoice (p.apvts, "preset", 0);
    REQUIRE (std::abs (getFloat (p.apvts, "denoiseAmount") - 40.0f) < 0.05f);
}
