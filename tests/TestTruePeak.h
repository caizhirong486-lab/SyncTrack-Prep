// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>

/**
 * True-peak measurement for tests (metric E).
 *
 * The limiter estimates inter-sample peaks itself, so it must be judged by an
 * independent measurement. juce::dsp::Oversampling is unusable as that judge:
 * its half-band FIR rings, and on a pure sine whose analytic peak is exactly
 * -6.021 dB it reports up to -5.50 dB (+0.5 dB) between 9 and 21 kHz.
 *
 * This is a plain linear-phase Kaiser-windowed sinc reconstruction at 8x.
 * Residual error is dominated by the 8x grid missing the continuous maximum:
 * at most -0.03 dB at 21 kHz, i.e. it never overestimates. `judgeBiasDb` below
 * pins that claim in a test.
 */
namespace test_tp
{
constexpr int oversample = 8;
constexpr int tapsPerPhase = 48;

inline double bessel_i0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k)
    {
        term *= (x * x) / (4.0 * (double) k * (double) k);
        sum += term;
        if (term < 1.0e-14 * sum)
            break;
    }
    return sum;
}

inline double sinc (double x)
{
    if (std::abs (x) < 1.0e-12)
        return 1.0;
    const double px = juce::MathConstants<double>::pi * x;
    return std::sin (px) / px;
}

/** Polyphase interpolation taps, built once per process. */
inline const std::vector<float>& taps()
{
    static const std::vector<float> t = []
    {
        constexpr double beta = 9.0;
        std::vector<float> out ((size_t) (oversample * tapsPerPhase), 0.0f);
        const double centre = (double) (tapsPerPhase / 2 - 1);
        const double i0b = bessel_i0 (beta);

        for (int p = 0; p < oversample; ++p)
        {
            const double offset = (double) p / (double) oversample;
            double sum = 0.0;
            for (int k = 0; k < tapsPerPhase; ++k)
            {
                const double r = 2.0 * (double) k / (double) (tapsPerPhase - 1) - 1.0;
                const double w = bessel_i0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / i0b;
                const double h = sinc ((double) k - centre - offset) * w;
                out[(size_t) (p * tapsPerPhase + k)] = (float) h;
                sum += h;
            }
            if (std::abs (sum) > 1.0e-9)
                for (int k = 0; k < tapsPerPhase; ++k)
                    out[(size_t) (p * tapsPerPhase + k)] /= (float) sum;
        }
        return out;
    }();
    return t;
}
}

inline float measureTruePeakDb (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    juce::ignoreUnused (sampleRate);

    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh < 1 || n <= 0)
        return -100.0f;

    const auto& h = test_tp::taps();
    constexpr int T = test_tp::tapsPerPhase;
    constexpr int M = test_tp::oversample;
    constexpr int lead = T / 2 - 1;

    float peak = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* x = buffer.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
        {
            for (int p = 0; p < M; ++p)
            {
                const float* hp = h.data() + p * T;
                double acc = 0.0;
                for (int k = 0; k < T; ++k)
                {
                    const int idx = i - lead + k;
                    if (idx >= 0 && idx < n)
                        acc += (double) x[idx] * (double) hp[k];
                }
                peak = juce::jmax (peak, (float) std::abs (acc));
            }
        }
    }

    return juce::Decibels::gainToDecibels (peak, -100.0f);
}
