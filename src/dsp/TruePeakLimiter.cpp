// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TruePeakLimiter.h"
#include <cmath>

namespace
{
/** Estimator error budget: keeps the measured peak under the ceiling. */
constexpr float safetyMarginDb = 0.2f;
/** Hard-clip safety, well below the gain target. The clip is a last resort for
    when the estimator under-reads; a clipped flat top overshoots by several
    tenths of a dB in the interpolated true-peak measurement, so the clip must
    sit low enough that its own overshoot stays under the ceiling. The gain
    path normally lands at targetLin (-1.2 dB) and never touches the clip. */
constexpr float clipMarginDb = 0.7f;

float sincf (float x)
{
    if (std::abs (x) < 1.0e-6f)
        return 1.0f;
    const float px = juce::MathConstants<float>::pi * x;
    return std::sin (px) / px;
}
}

void TruePeakLimiter::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;

    // Windowed-sinc polyphase taps: phase p reconstructs the sub-sample at
    // offset p/numPhases between two input samples.
    taps.assign ((size_t) (numPhases * numTaps), 0.0f);
    const float centre = (float) (numTaps / 2 - 1);
    for (int p = 0; p < numPhases; ++p)
    {
        const float offset = (float) p / (float) numPhases;
        float sum = 0.0f;
        for (int k = 0; k < numTaps; ++k)
        {
            const float x = (float) k - centre - offset;
            const float w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                   * ((float) k + 0.5f) / (float) numTaps);
            const float h = sincf (x) * w;
            taps[(size_t) (p * numTaps + k)] = h;
            sum += h;
        }
        // Unity DC gain per phase so the estimate is not biased.
        if (std::abs (sum) > 1.0e-6f)
            for (int k = 0; k < numTaps; ++k)
                taps[(size_t) (p * numTaps + k)] /= sum;
    }

    const int cap = lookaheadSamples + 1;
    delayL.assign ((size_t) cap, 0.0f);
    delayR.assign ((size_t) cap, 0.0f);
    tpBuf.assign ((size_t) cap, 0.0f);
    window.assign ((size_t) cap, 0);
    histL.assign ((size_t) numTaps, 0.0f);
    histR.assign ((size_t) numTaps, 0.0f);

    reset();
}

void TruePeakLimiter::reset()
{
    gain = 1.0f;
    ceilingLin = juce::Decibels::decibelsToGain (params.ceilingDb);
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    std::fill (tpBuf.begin(), tpBuf.end(), 0.0f);
    std::fill (histL.begin(), histL.end(), 0.0f);
    std::fill (histR.begin(), histR.end(), 0.0f);
    histWrite = 0;
    windowHead = windowCount = 0;
    sampleIndex = 0;
}

void TruePeakLimiter::pushHistory (float l, float r)
{
    histL[(size_t) histWrite] = l;
    histR[(size_t) histWrite] = r;
    if (++histWrite >= numTaps)
        histWrite = 0;
}

float TruePeakLimiter::estimateTruePeak() const
{
    float peak = 0.0f;
    for (int k = 0; k < numTaps; ++k)
        peak = juce::jmax (peak, std::abs (histL[(size_t) k]), std::abs (histR[(size_t) k]));

    for (int p = 1; p < numPhases; ++p)
    {
        float accL = 0.0f, accR = 0.0f;
        const float* h = taps.data() + p * numTaps;
        int idx = histWrite;
        for (int k = 0; k < numTaps; ++k)
        {
            accL += histL[(size_t) idx] * h[k];
            accR += histR[(size_t) idx] * h[k];
            if (++idx >= numTaps)
                idx = 0;
        }
        peak = juce::jmax (peak, std::abs (accL), std::abs (accR));
    }
    return peak;
}

void TruePeakLimiter::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh < 1 || n <= 0 || ! params.enabled)
        return;

    ceilingLin = juce::Decibels::decibelsToGain (params.ceilingDb);
    const float targetLin = ceilingLin * juce::Decibels::decibelsToGain (-safetyMarginDb);
    const float relCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * juce::jmax (0.001f, params.releaseMs * 0.001f)));

    auto* L = buffer.getWritePointer (0);
    auto* R = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    const int cap = (int) tpBuf.size();

    for (int i = 0; i < n; ++i)
    {
        const float l = L[i];
        const float r = R != nullptr ? R[i] : l;

        pushHistory (l, r);
        const float tp = estimateTruePeak();

        const long long idx = sampleIndex;
        const int pos = (int) (idx % cap);
        tpBuf[(size_t) pos] = tp;
        delayL[(size_t) pos] = l;
        delayR[(size_t) pos] = r;

        // Sliding-window maximum over [outIdx, outIdx + lookahead]. The window
        // spans exactly cap samples, so the deque never exceeds cap entries.
        while (windowCount > 0
               && tpBuf[(size_t) (window[(size_t) ((windowHead + windowCount - 1) % cap)] % cap)] <= tp)
            --windowCount;
        window[(size_t) ((windowHead + windowCount) % cap)] = idx;
        ++windowCount;

        const long long outIdx = idx - lookaheadSamples;
        while (windowCount > 0 && window[(size_t) windowHead] < outIdx)
        {
            windowHead = (windowHead + 1) % cap;
            --windowCount;
        }

        ++sampleIndex;

        if (outIdx < 0)
        {
            // Priming the look-ahead: no output yet, hold silence.
            L[i] = 0.0f;
            if (R != nullptr)
                R[i] = 0.0f;
            continue;
        }

        const float windowMax = tpBuf[(size_t) (window[(size_t) windowHead] % cap)];
        float needed = 1.0f;
        if (windowMax * gain > targetLin && windowMax > 1.0e-12f)
            needed = targetLin / (windowMax + 1.0e-12f);

        if (needed < gain)
            gain = needed; // look-ahead lets the reduction land before the peak
        else
            gain += relCoef * (1.0f - gain);

        const int outPos = (int) (outIdx % cap);
        const float clipLin = ceilingLin * juce::Decibels::decibelsToGain (-clipMarginDb);
        const float outL = juce::jlimit (-clipLin, clipLin, delayL[(size_t) outPos] * gain);
        const float outR = juce::jlimit (-clipLin, clipLin, delayR[(size_t) outPos] * gain);
        L[i] = outL;
        if (R != nullptr)
            R[i] = outR;
    }
}
