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
        float sceneOffsetDb = -4.0f;    // scene level to threshold distance
        float transientReleaseMs = 40.0f; // fast release while content is spiky
    };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParams (const Params& p) { params = p; }

    void process (juce::AudioBuffer<float>& buffer);

    float getLastGainReductionDb() const { return lastGrDb; }

    /** Reference scene level in dB for threshold coupling (post-leveler).
        Until called, the compressor falls back to params.thresholdDb. */
    void setSceneLevelDb (float db) { sceneLevelDb = db; sceneLevelValid = true; }

private:
    Params params;
    double sampleRate = 48000.0;
    float envelope = 0.0f;
    float lastGrDb = 0.0f;

    // Scene-adaptive threshold
    float sceneLevelDb = -100.0f;
    bool sceneLevelValid = false;

    // Release discrimination: short peak vs slow RMS envelope
    float peakEnv = 0.0f;
    float rmsEnv = 0.0f;
    float crestAvg = 0.0f; // smoothed crest in dB
    bool transientMode = false;

    static constexpr float peakTauSec = 0.003f;
    static constexpr float rmsTauSec = 0.030f;
    static constexpr float crestTauSec = 0.050f;
    // Measured through this detector: a steady tone sits at ~4 dB crest,
    // continuous noise at ~10-11 dB, real bangs higher. 8/5 splits tone from
    // spiky content; hysteresis keeps the mode from flapping.
    static constexpr float crestEnterDb = 8.0f;
    static constexpr float crestExitDb = 5.0f;
    static constexpr float thrCeilDb = -1.0f; // never ride up into clipping territory
};
