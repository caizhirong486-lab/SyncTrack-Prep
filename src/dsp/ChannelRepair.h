// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_dsp/juce_dsp.h>

/**
 * Channel repair for production tracks.
 *
 * Critical rule (test6): NEVER mid-sum uncorrelated stereo — that creates
 * harsh "electrical" HF noise. Only hard-fill true one-sided silence;
 * dialogue mono uses dominant-channel dual-mono when sides fight.
 */
class ChannelRepair
{
public:
    struct Params
    {
        bool enabled = true;
        /** Soft/Strong dialogue: center image without phase-sum noise. */
        bool dialogueMono = true;
        float strength = 0.6f;
        float activityDb = -60.0f;
        float balanceTauSec = 0.35f;
        float maxBalanceDb = 18.0f;
        /** If side/mid energy above this, treat as dual-mono mess → pick dominant. */
        float sideFightRatio = 0.65f;
    };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParams (const Params& p) { params = p; }
    const Params& getParams() const { return params; }

    void process (juce::AudioBuffer<float>& buffer);

    enum class Mode { silence, leftOnly, rightOnly, stereo, dominantMono };
    Mode getLastMode() const { return lastMode; }

private:
    Params params;
    double sampleRate = 48000.0;
    float envL = 0.0f;
    float envR = 0.0f;
    float envMid = 0.0f;
    float envSide = 0.0f;
    float balanceGainL = 1.0f;
    float balanceGainR = 1.0f;
    Mode lastMode = Mode::silence;

    static float dbToGain (float db) { return juce::Decibels::decibelsToGain (db); }
    static float gainToDb (float g) { return juce::Decibels::gainToDecibels (g, -120.0f); }
};
