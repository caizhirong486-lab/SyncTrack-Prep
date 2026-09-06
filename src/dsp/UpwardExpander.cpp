// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpwardExpander.h"

#include <cmath>

void UpwardExpander::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    envDb.assign ((size_t) juce::jmax (1, (int) spec.numChannels), -100.0f);
    attackCoef = 1.0f - std::exp (-1.0f / (params.attackMs * 0.001f * (float) sampleRate));
    releaseCoef = 1.0f - std::exp (-1.0f / (params.releaseMs * 0.001f * (float) sampleRate));
    currentGainDb = 0.0f;
    reset();
}

void UpwardExpander::reset()
{
    for (auto& e : envDb)
        e = -100.0f;
    currentGainDb = 0.0f;
}

void UpwardExpander::setParams (const Params& p)
{
    const bool timingChanged = p.attackMs != params.attackMs || p.releaseMs != params.releaseMs;
    params = p;
    if (timingChanged)
    {
        attackCoef = 1.0f - std::exp (-1.0f / (params.attackMs * 0.001f * (float) sampleRate));
        releaseCoef = 1.0f - std::exp (-1.0f / (params.releaseMs * 0.001f * (float) sampleRate));
    }
}

void UpwardExpander::process (juce::AudioBuffer<float>& buffer)
{
    if (! params.enabled || buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0)
        return;

    const float range = juce::jmax (0.0f, params.rangeDb);
    const float slope = juce::jlimit (0.01f, 0.99f, 1.0f - 1.0f / juce::jmax (1.0f, params.ratio));

    const int n = buffer.getNumSamples();
    const int chs = juce::jmin ((int) envDb.size(), buffer.getNumChannels());

    float frameGain = currentGainDb;
    for (int i = 0; i < n; ++i)
    {
        // Peak envelope across channels, in dB
        float peak = 0.0f;
        for (int ch = 0; ch < chs; ++ch)
            peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));
        const float peakDb = juce::Decibels::gainToDecibels (peak, -100.0f);

        for (int ch = 0; ch < chs; ++ch)
        {
            float& e = envDb[(size_t) ch];
            e += (peakDb > e ? attackCoef : releaseCoef) * (peakDb - e);
        }

        // Use the fastest channel envelope for the gain computation
        float env = envDb[0];
        for (int ch = 1; ch < chs; ++ch)
            env = juce::jmax (env, envDb[(size_t) ch]);

        const float sceneNow = sceneStream != nullptr
            ? (*sceneStream)[(size_t) i] + sceneLevelDb.load (std::memory_order_relaxed)
            : sceneLevelDb.load (std::memory_order_relaxed);
        const float thr = sceneNow + params.sceneOffsetDb;
        const float over = env - thr; // < 0 below threshold
        const float targetDb = over < 0.0f
            ? juce::jmin (range, -over * slope)
            : 0.0f;
        // Fast enough per-sample smoothing on top of the 5/150 ms envelope
        frameGain += 0.35f * (targetDb - frameGain);

        const float g = juce::Decibels::decibelsToGain (frameGain);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
    }
    currentGainDb = frameGain;
    lastGainDb.store (frameGain, std::memory_order_relaxed);
}
