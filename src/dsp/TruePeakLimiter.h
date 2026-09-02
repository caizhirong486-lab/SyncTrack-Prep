// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>

/**
 * Brickwall limiter with a short look-ahead and a 4x polyphase true-peak
 * estimate.
 *
 * The MVP version estimated inter-sample peaks from linear interpolation and
 * applied gain to the current sample only, so it could not hold the ceiling:
 * measured output reached -0.49 dBTP at 15 kHz and the full chain exceeded
 * 0 dBFS. Linear interpolation underestimates inter-sample peaks, and without
 * look-ahead the gain for a peak arrives one sample too late.
 *
 * Now the estimate uses a windowed-sinc polyphase interpolator and the gain is
 * derived from a sliding maximum over the look-ahead window, so the reduction
 * is already in place when the peak reaches the output.
 */
class TruePeakLimiter
{
public:
    /** 4x oversampled estimate: 4 phases, 32 taps each.
        32 taps keep the interpolator flat close to Nyquist; a shorter window
        rolls off and underestimates near-Nyquist peaks. */
    static constexpr int numPhases = 4;
    static constexpr int numTaps = 32;
    /** ~1.3 ms at 48 kHz — long enough to cover the estimator's own delay. */
    static constexpr int lookaheadSamples = 64;

    struct Params
    {
        bool enabled = true;
        float ceilingDb = -1.0f;
        float releaseMs = 50.0f;
    };

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setParams (const Params& p) { params = p; }

    void process (juce::AudioBuffer<float>& buffer);

    int getLatencySamples() const { return params.enabled ? lookaheadSamples : 0; }

private:
    Params params;
    double sampleRate = 48000.0;
    float gain = 1.0f;
    float ceilingLin = 0.89125f;

    /** Interpolator taps [phase][tap], built once in prepare. */
    std::vector<float> taps;

    /** Delay line for the audio, plus per-sample true-peak estimates. */
    std::vector<float> delayL, delayR, tpBuf;
    /** Monotonic circular deque of absolute indices for the window maximum. */
    std::vector<long long> window;
    int windowHead = 0, windowCount = 0;
    long long sampleIndex = 0;

    std::vector<float> histL, histR;
    int histWrite = 0;

    void pushHistory (float l, float r);
    float estimateTruePeak() const;
};
