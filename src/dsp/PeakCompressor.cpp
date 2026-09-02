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
}

void PeakCompressor::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0 || ! params.enabled)
        return;

    const float thrDb = params.thresholdDb;
    const float s = juce::jlimit (0.0f, 1.0f, params.strength);
    const float ratio = 1.0f + (juce::jmax (1.01f, params.ratio) - 1.0f) * s;

    const float attSec = juce::jmax (0.00005f, params.attackMs * 0.001f);
    const float relSec = juce::jmax (0.001f, params.releaseMs * 0.001f);
    const float attCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * attSec));
    const float relCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * relSec));
    const float makeupDb = params.makeupDb * s;

    float maxGr = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));

        if (peak > envelope)
            envelope += attCoef * (peak - envelope);
        else
            envelope += relCoef * (peak - envelope);

        const float envDb = juce::Decibels::gainToDecibels (envelope, -100.0f);
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
