// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PeakCompressor.h"
#include <cmath>

void PeakCompressor::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    reset();
}

void PeakCompressor::reset()
{
    envelope = 0.0f;
    lastGrDb = 0.0f;
    sceneLevelDb = -100.0f;
    sceneLevelValid = false;
    peakEnv = 0.0f;
    rmsEnv = 0.0f;
    crestAvg = 0.0f;
    transientMode = false;
}

void PeakCompressor::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0 || ! params.enabled)
        return;

    // Threshold coupling: the leveler normalizes the scene, so the compressor
    // rides scene - 4 dB instead of a fixed threshold. The scene-relative
    // structure self-bounds (the signal's own level sits ~4 dB over it), so no
    // lower clamp is needed — only the -1 dBFS ceiling keeps a loud scene from
    // pushing the threshold into clipping territory. Without a scene level the
    // legacy params.thresholdDb applies unchanged.
    const float s = juce::jlimit (0.0f, 1.0f, params.strength);
    const float ratio = 1.0f + (juce::jmax (1.01f, params.ratio) - 1.0f) * s;

    const float attSec = juce::jmax (0.00005f, params.attackMs * 0.001f);
    const float relSustSec = juce::jmax (0.001f, params.releaseMs * 0.001f);
    const float relTransSec = juce::jmax (0.001f, params.transientReleaseMs * 0.001f);
    const float attCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * attSec));
    const float relSustCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * relSustSec));
    const float relTransCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * relTransSec));
    const float peakCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * peakTauSec));
    const float rmsCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * rmsTauSec));
    const float crestCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * crestTauSec));
    const float makeupDb = params.makeupDb * s;

    float maxGr = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));

        if (peak > envelope)
        {
            envelope += attCoef * (peak - envelope);
        }
        else
        {
            // Release time depends on content: only the coefficient switches,
            // the envelope itself is continuous, so mode flips cannot zipper.
            const float relCoef = transientMode ? relTransCoef : relSustCoef;
            envelope += relCoef * (peak - envelope);
        }

        peakEnv += peakCoef * (peak - peakEnv);
        rmsEnv += rmsCoef * (peak - rmsEnv);
        const float crestDb = juce::Decibels::gainToDecibels (peakEnv, -100.0f)
                            - juce::Decibels::gainToDecibels (juce::jmax (rmsEnv, 1.0e-6f), -100.0f);
        crestAvg += crestCoef * (crestDb - crestAvg);

        // Spiky content (door slams, footsteps) wants the envelope back fast;
        // sustained speech would pump on a 40 ms release. Same hysteresis
        // pattern as the Leveler's mod gate.
        if (! transientMode && crestAvg > crestEnterDb)
            transientMode = true;
        else if (transientMode && crestAvg < crestExitDb)
            transientMode = false;

        const float envDb = juce::Decibels::gainToDecibels (envelope, -100.0f);
        // Per-sample scene tracking keeps the threshold stream itself
        // block-size independent (a per-block snapshot leaks host block sizes
        // into the output — caught by the block-invariance test).
        const float thrDb = sceneStream != nullptr
            ? juce::jmin ((*sceneStream)[(size_t) i] + sceneLevelDb + params.sceneOffsetDb, thrCeilDb)
            : (sceneLevelValid
                ? juce::jmin (sceneLevelDb + params.sceneOffsetDb, thrCeilDb)
                : params.thresholdDb);
        float grDb = 0.0f;
        if (envDb > thrDb && ratio > 1.0f)
        {
            const float overDb = envDb - thrDb;
            const float compressedOver = overDb / ratio;
            grDb = compressedOver - overDb; // negative
        }

        grDb *= s;
        grDb += makeupDb;

        const float gain = juce::Decibels::decibelsToGain (grDb);
        maxGr = juce::jmin (maxGr, grDb);

        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= gain;
    }

    lastGrDb = maxGr;
}
