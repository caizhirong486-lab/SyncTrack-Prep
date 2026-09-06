// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Leveler.h"
#include <cmath>

void Leveler::prepare (const juce::dsp::ProcessSpec& spec)
{
    // Sized here so process() never allocates and the downstream stages can
    // index the whole block unconditionally.
    sceneStreamBuf.assign ((size_t) juce::jmax (1u, spec.maximumBlockSize),
                           juce::Decibels::gainToDecibels (0.0f, -100.0f));
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    reset();
}

void Leveler::reset()
{
    fastEnv = 0.0f;
    slowEnv = 0.0f;
    gain = 1.0f;
    lastGainDb = 0.0f;
    fastAvg = 0.0f;
    modAvg = 0.0f;
    noiseHold = false;
}

void Leveler::process (juce::AudioBuffer<float>& buffer)
{
    // Contract: after process() the first getNumSamples() entries of the scene
    // stream are valid, disabled or not — the compressor and expander index it
    // unconditionally.
    ensureSceneStream (buffer.getNumSamples());
    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0 || ! params.enabled)
        return;

    const float s = juce::jlimit (0.0f, 1.0f, params.strength);
    const float depth = 0.40f + 0.60f * s;
    const float maxGain = juce::Decibels::decibelsToGain (params.maxGainDb * depth);
    const float minGain = juce::Decibels::decibelsToGain (-params.maxAttenDb * depth);
    const float target = juce::Decibels::decibelsToGain (params.targetDb);
    const float gate = juce::Decibels::decibelsToGain (params.gateDb);
    const float deepSilence = gate * 0.15f;

    const float fastCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * juce::jmax (0.01f, params.fastTauSec)));
    const float slowCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * juce::jmax (0.5f, params.slowTauSec)));
    const float boostCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * juce::jmax (0.08f, params.releaseSec)));
    const float cutCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * juce::jmax (0.05f, params.attackSec)));
    const float silenceCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.05f));
    const float fastAvgCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * fastAvgTauSec));
    const float modCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * modTauSec));
    const float holdCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * noiseHoldTauSec));

    for (int i = 0; i < n; ++i)
    {
        float sq = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float x = buffer.getSample (ch, i);
            sq += x * x;
        }
        const float amp = std::sqrt (sq / (float) numCh + 1.0e-20f);
        fastEnv += fastCoef * (amp - fastEnv);
        slowEnv += slowCoef * (amp - slowEnv);

        // Modulation estimate: how far the fast envelope swings from its own
        // short-time mean. Steady noise ~0.03, syllabic speech >= 0.3.
        fastAvg += fastAvgCoef * (amp - fastAvg);
        const float dev = std::abs (fastEnv - fastAvg) / juce::jmax (fastAvg, 1.0e-6f);
        modAvg += modCoef * (dev - modAvg);
        // Low modulation alone is not enough: a sustained vowel modulates
        // like noise but sits at dialogue level, so it must keep its gain.
        const float quietFloor = juce::Decibels::decibelsToGain (quietFloorDb);
        if (modAvg < modEnterThr && fastEnv < quietFloor)
            noiseHold = true;
        else if (modAvg > modExitThr)
            noiseHold = false;

        float targetGain = 1.0f;
        float gCoef = cutCoef;

        if (fastEnv <= deepSilence)
        {
            targetGain = 1.0f;
            gCoef = silenceCoef;
        }
        else if (slowEnv <= gate && fastEnv <= gate)
        {
            targetGain = 1.0f;
            gCoef = cutCoef;
        }
        else if (noiseHold)
        {
            // Steady low-modulation frame: interference / room tone, not
            // dialogue. Do not chase it toward the target; drift back to
            // unity slowly so the transitions do not pump.
            targetGain = 1.0f;
            gCoef = holdCoef;
        }
        else
        {
            // Scene target from slow envelope; peak guard from fast
            const float sceneEnv = juce::jmax (slowEnv, gate * 1.5f);
            float sceneGain = target / juce::jmax (sceneEnv, 1.0e-8f);
            sceneGain = juce::jlimit (minGain, maxGain, sceneGain);

            // If fast peak much louder than scene, reduce applied gain
            if (fastEnv > sceneEnv * 2.5f && sceneEnv > 1.0e-8f)
            {
                const float guard = sceneEnv * 2.5f / juce::jmax (fastEnv, 1.0e-8f);
                sceneGain *= juce::jlimit (0.25f, 1.0f, guard);
            }

            targetGain = 1.0f + depth * (sceneGain - 1.0f);
            targetGain = juce::jlimit (minGain, maxGain, targetGain);
            gCoef = (targetGain > gain) ? boostCoef : cutCoef;
        }

        gain += gCoef * (targetGain - gain);

        sceneStreamBuf[(size_t) i] = juce::Decibels::gainToDecibels (slowEnv * gain, -100.0f);

        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= gain;
    }

    lastGainDb = juce::Decibels::gainToDecibels (gain, -60.0f);
}
