// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>

#include <juce_dsp/juce_dsp.h>
#include <vector>

/**
 * Upward expander for quiet-passage lift ("dynamics outreach").
 *
 * Threshold rides the post-leveler scene level (sceneOffsetDb below it), the
 * same convention as PeakCompressor, so the expansion survives the output
 * makeup gain instead of drifting with the knob. Gain law below threshold:
 * boost = (1 - 1/ratio) * (threshold - level), capped at rangeDb. The
 * on/off control experiment (plan Step 5) may converge rangeDb to 0 — the
 * defaults are seeds, not conclusions.
 */
class UpwardExpander
{
public:
    struct Params
    {
        bool enabled = true;
        float ratio = 1.5f;          // 1:n upward ratio
        float rangeDb = 8.0f;        // max boost
        float attackMs = 5.0f;
        float releaseMs = 150.0f;
        float sceneOffsetDb = -20.0f; // threshold = sceneLevel + offset
    };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParams (const Params& p);
    void setSceneLevelDb (float db) { sceneLevelDb.store (db, std::memory_order_relaxed); }
    /** Bind the leveler's per-sample scene stream (see PeakCompressor). When
        bound, the threshold is sceneSource + sceneLevelDb + sceneOffsetDb;
        unbound, sceneLevelDb + sceneOffsetDb. */
    void bindSceneSource (const std::vector<float>* stream) { sceneStream = stream; }

    void process (juce::AudioBuffer<float>& buffer);

    float getLastGainDb() const { return lastGainDb.load (std::memory_order_relaxed); }

private:
    Params params;
    double sampleRate = 48000.0;

    // Per-channel smoothed envelope (dB), peak-style
    std::vector<float> envDb;
    float attackCoef = 0.0f, releaseCoef = 0.0f;
    float currentGainDb = 0.0f;

    std::atomic<float> sceneLevelDb { -18.0f };
    const std::vector<float>* sceneStream = nullptr; // per-sample scene of the current block
    std::atomic<float> lastGainDb { 0.0f };
};
