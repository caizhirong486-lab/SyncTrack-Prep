// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ChannelRepair.h"
#include "NoiseSuppressor.h"
#include "Leveler.h"
#include "PeakCompressor.h"
#include "TruePeakLimiter.h"

/**
 * The Soft / Strong / Clean strategy table.
 *
 * Single source of truth for the processor, the offline render tool and the
 * tests. These three used to keep private copies that had already drifted
 * apart (Strong strength 0.6 vs 0.75, HPF 80 vs 70 Hz), so offline and test
 * results no longer described the plugin.
 */
namespace Presets
{
enum Index { soft = 0, strong = 1, clean = 2 };

constexpr int count = 3;

/** Full DSP configuration for one preset. */
struct Chain
{
    ChannelRepair::Params channelRepair;
    NoiseSuppressor::Params noiseSuppressor;
    Leveler::Params leveler;
    PeakCompressor::Params peakCompressor;
    TruePeakLimiter::Params truePeakLimiter;
};

inline const char* name (int index)
{
    switch (index)
    {
        case soft:  return "Soft";
        case clean: return "Clean";
        default:    return "Strong";
    }
}

/** Denoise default for a preset; the user may override it afterwards. */
inline bool denoiseDefault (int index) { return index == clean; }

inline Chain chainFor (int presetIndex, bool denoiseOn)
{
    const int preset = juce::jlimit (0, count - 1, presetIndex);

    float maxGainDb = 14.0f;
    float maxAttenDb = 10.0f;
    float slowTau = 3.0f;
    float levelStrength = 0.65f;
    float levelGate = -46.0f;
    float compRatio = 2.5f;
    float compThresh = -6.0f;
    float denoiseAmount = 0.45f;

    if (preset == soft)
    {
        maxGainDb = 12.0f;
        maxAttenDb = 8.0f;
        slowTau = 3.5f;
        levelStrength = 0.55f;
        levelGate = -46.0f;
        compRatio = 2.0f;
        compThresh = -4.0f;
        denoiseAmount = 0.4f;
    }
    else if (preset == strong)
    {
        maxGainDb = 18.0f;
        maxAttenDb = 12.0f;
        slowTau = 2.5f;
        levelStrength = 0.75f;
        levelGate = -48.0f;
        compRatio = 3.0f;
        compThresh = -6.0f;
        denoiseAmount = 0.45f;
    }
    else // Clean — denoise carries the noise floor, so the leveler stays light
    {
        maxGainDb = 8.0f;
        maxAttenDb = 8.0f;
        slowTau = 4.0f;
        levelStrength = 0.40f;
        levelGate = -42.0f;
        compRatio = 2.0f;
        compThresh = -5.0f;
        denoiseAmount = 0.55f; // 0.75 was too aggressive on speech
    }

    Chain c;

    c.channelRepair.enabled = true;
    c.channelRepair.dialogueMono = true;
    c.channelRepair.strength = levelStrength;
    c.channelRepair.sideFightRatio = 0.55f;
    c.channelRepair.activityDb = -60.0f;

    c.noiseSuppressor.enabled = denoiseOn;
    c.noiseSuppressor.amount = denoiseAmount;
    c.noiseSuppressor.hpfHz = 70.0f;
    // Soft spectral subtraction + strong speech-band protect (anti-distortion)
    c.noiseSuppressor.overSubtract = denoiseOn ? (preset == clean ? 1.05f : 1.0f) : 1.0f;
    c.noiseSuppressor.speechProtect = denoiseOn ? (preset == clean ? 0.75f : 0.65f) : 0.0f;

    c.leveler.enabled = true;
    c.leveler.strength = levelStrength;
    c.leveler.targetDb = -18.0f;
    c.leveler.maxGainDb = maxGainDb;
    c.leveler.maxAttenDb = maxAttenDb;
    c.leveler.gateDb = levelGate;
    c.leveler.fastTauSec = 0.04f;
    c.leveler.slowTauSec = slowTau;
    c.leveler.attackSec = 0.12f;
    c.leveler.releaseSec = 0.55f;

    c.peakCompressor.enabled = true; // always-on safety
    c.peakCompressor.strength = 1.0f;
    c.peakCompressor.ratio = compRatio;
    c.peakCompressor.thresholdDb = compThresh;
    c.peakCompressor.attackMs = 3.0f;
    c.peakCompressor.releaseMs = 100.0f;

    c.truePeakLimiter.enabled = true;
    c.truePeakLimiter.ceilingDb = -1.0f;

    return c;
}
}
