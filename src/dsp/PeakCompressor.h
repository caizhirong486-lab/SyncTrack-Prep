// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_dsp/juce_dsp.h>

/** Fast downward compressor for peaks / bangs. */
class PeakCompressor
{
public:
    struct Params
    {
        bool enabled = true;
        float thresholdDb = -12.0f;
        float ratio = 4.0f;
        float attackMs = 5.0f;
        float releaseMs = 80.0f;
        float makeupDb = 0.0f;
        float strength = 0.6f; // scales effective ratio toward 1
    };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParams (const Params& p) { params = p; }

    void process (juce::AudioBuffer<float>& buffer);

    float getLastGainReductionDb() const { return lastGrDb; }

private:
    Params params;
    double sampleRate = 48000.0;
    float envelope = 0.0f;
    float lastGrDb = 0.0f;
};
