// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_dsp/juce_dsp.h>

/** Single-knob tone stage: tilt (low/high shelf) plus a presence push.

    tone > 0 brightens and pushes dialogue forward (3 kHz bell); tone < 0
    warms and recedes. Zero is bit-transparent: at 0 dB gain every RBJ shelf /
    bell here degenerates to H(z) = 1 exactly, and process() short-circuits.

    Coefficients are hand-rolled RBJ biquads rather than
    IIR::Coefficients::make* — see pitfall.md: audio-thread allocation. They
    are recomputed only when tone actually changes.
*/
class ToneShaper
{
public:
    struct Params
    {
        bool enabled = true;
        float tone = 0.0f; // -1..1
    };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParams (const Params& p);

    void process (juce::AudioBuffer<float>& buffer);

    int getLatencySamples() const { return 0; }

private:
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    };

    static constexpr int numChannels = 2;
    static constexpr float shelfLowHz = 700.0f;
    static constexpr float shelfHighHz = 1500.0f;
    static constexpr float presenceHz = 3000.0f;
    static constexpr float shelfQ = 0.7071f;
    static constexpr float presenceQ = 0.8f;
    static constexpr float maxShelfGainDb = 3.0f;
    static constexpr float maxPresenceGainDb = 3.0f;

    void rebuildCoefficients();

    Params params;
    double sampleRate = 48000.0;

    Biquad sections[numChannels][3]; // low shelf, high shelf, presence per channel
};
