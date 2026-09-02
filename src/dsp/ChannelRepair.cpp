// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChannelRepair.h"
#include <cmath>

void ChannelRepair::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    reset();
}

void ChannelRepair::reset()
{
    envL = envR = envMid = envSide = 0.0f;
    balanceGainL = balanceGainR = 1.0f;
    lastMode = Mode::silence;
}

void ChannelRepair::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh < 2 || n <= 0 || ! params.enabled)
        return;

    auto* L = buffer.getWritePointer (0);
    auto* R = buffer.getWritePointer (1);

    const float thr = dbToGain (params.activityDb);
    const float envTau = 0.012f;
    const float envCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * envTau));
    const float balTau = juce::jmax (0.05f, params.balanceTauSec);
    const float balCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * balTau));
    const float maxBalDb = params.maxBalanceDb * juce::jlimit (0.0f, 1.0f, params.strength);
    const float fight = juce::jmax (0.2f, params.sideFightRatio);

    // Block peak every ~5 ms for reliable digital-silence detection
    const int probe = juce::jmax (1, (int) std::lround (sampleRate * 0.005));
    float blockPeakL = 0.0f, blockPeakR = 0.0f;
    int probeCount = 0;
    bool blockLeftOnly = false, blockRightOnly = false, blockSilent = false;

    for (int i = 0; i < n; ++i)
    {
        float l = L[i];
        float r = R[i];
        const float absL = std::abs (l);
        const float absR = std::abs (r);

        envL += envCoef * (absL - envL);
        envR += envCoef * (absR - envR);

        const float midS = 0.5f * (l + r);
        const float sideS = 0.5f * (l - r);
        envMid += envCoef * (std::abs (midS) - envMid);
        envSide += envCoef * (std::abs (sideS) - envSide);

        blockPeakL = juce::jmax (blockPeakL, absL);
        blockPeakR = juce::jmax (blockPeakR, absR);
        if (++probeCount >= probe)
        {
            const bool aL = blockPeakL > thr;
            const bool aR = blockPeakR > thr;
            blockSilent = ! aL && ! aR;
            blockLeftOnly = aL && ! aR;
            blockRightOnly = aR && ! aL;
            blockPeakL = blockPeakR = 0.0f;
            probeCount = 0;
        }

        // ---- 1) True one-sided silence: safe dual-mono copy (no sum) ----
        if (blockLeftOnly || (envL > thr && envR <= thr && blockPeakR <= thr))
        {
            lastMode = Mode::leftOnly;
            R[i] = l; // full copy, not *0.707
            balanceGainL = balanceGainR = 1.0f;
            continue;
        }
        if (blockRightOnly || (envR > thr && envL <= thr && blockPeakL <= thr))
        {
            lastMode = Mode::rightOnly;
            L[i] = r;
            balanceGainL = balanceGainR = 1.0f;
            continue;
        }
        if (blockSilent || (envL <= thr && envR <= thr))
        {
            lastMode = Mode::silence;
            balanceGainL += balCoef * (1.0f - balanceGainL);
            balanceGainR += balCoef * (1.0f - balanceGainR);
            continue;
        }

        // ---- 2) Dialogue mono policy (Soft/Strong) ----
        if (params.dialogueMono)
        {
            // Uncorrelated / dual-mono mess: side energy fights mid → pick dominant channel
            // Correlated stereo: gentle center (still avoid pure sum if side is hot)
            const bool sidesFight = envSide > fight * juce::jmax (envMid, thr);

            if (sidesFight || envL > 3.0f * envR || envR > 3.0f * envL)
            {
                lastMode = Mode::dominantMono;
                const float m = (envL >= envR) ? l : r;
                L[i] = m;
                R[i] = m;
            }
            else
            {
                // Similar level + not fighting → true center from mid is OK
                lastMode = Mode::dominantMono;
                L[i] = midS;
                R[i] = midS;
            }
            balanceGainL = balanceGainR = 1.0f;
            continue;
        }

        // ---- 3) Custom / auto: slow energy balance only (never mid-sum) ----
        lastMode = Mode::stereo;
        const float envLDb = gainToDb (envL + 1.0e-12f);
        const float envRDb = gainToDb (envR + 1.0e-12f);
        float balDb = 0.5f * (envLDb - envRDb);
        balDb = juce::jlimit (-maxBalDb, maxBalDb, balDb);
        const float targetL = dbToGain (-balDb);
        const float targetR = dbToGain (balDb);
        balanceGainL += balCoef * (targetL - balanceGainL);
        balanceGainR += balCoef * (targetR - balanceGainR);
        L[i] *= balanceGainL;
        R[i] *= balanceGainR;
    }
}
