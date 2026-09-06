// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ToneShaper.h"
#include <cmath>

void ToneShaper::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    reset();
    rebuildCoefficients();
}

void ToneShaper::reset()
{
    for (auto& ch : sections)
        for (auto& s : ch)
        {
            s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;
        }
}

void ToneShaper::setParams (const Params& p)
{
    const bool toneChanged = std::abs (p.tone - params.tone) > 1.0e-6f;
    params = p;
    if (toneChanged)
        rebuildCoefficients();
}

void ToneShaper::rebuildCoefficients()
{
    const double t = (double) juce::jlimit (-1.0f, 1.0f, params.tone);
    const double tiltDb = maxShelfGainDb * t;
    const double presenceDb = maxPresenceGainDb * juce::jmax (0.0, t);

    struct Spec { double f0, gainDb, q; };
    const Spec specs[3] = {
        { shelfLowHz,   -tiltDb,      shelfQ },   // cut lows when brightening
        { shelfHighHz,   tiltDb,      shelfQ },   // and lift highs
        { presenceHz,    presenceDb,  presenceQ } // dialogue push, bright side only
    };

    for (int section = 0; section < 3; ++section)
    {
        const auto& sp = specs[section];
        const double a = std::pow (10.0, sp.gainDb / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * sp.f0 / sampleRate;
        const double cw = std::cos (w0);
        const double sw = std::sin (w0);
        const double sqA = std::sqrt (a);

        double b0, b1, b2, a0, a1, a2;
        if (section == 2) // peaking bell
        {
            const double alpha = sw / (2.0 * sp.q);
            b0 = 1.0 + alpha * a;
            b1 = -2.0 * cw;
            b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a;
            a1 = -2.0 * cw;
            a2 = 1.0 - alpha / a;
        }
        else
        {
            const double alpha = sw / (2.0 * sp.q);
            const double twoSqrtAalpha = 2.0 * sqA * alpha;
            if (section == 0) // low shelf
            {
                b0 = a * ((a + 1.0) - (a - 1.0) * cw + twoSqrtAalpha);
                b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
                b2 = a * ((a + 1.0) - (a - 1.0) * cw - twoSqrtAalpha);
                a0 = (a + 1.0) + (a - 1.0) * cw + twoSqrtAalpha;
                a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cw);
                a2 = (a + 1.0) + (a - 1.0) * cw - twoSqrtAalpha;
            }
            else // high shelf
            {
                b0 = a * ((a + 1.0) + (a - 1.0) * cw + twoSqrtAalpha);
                b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cw);
                b2 = a * ((a + 1.0) + (a - 1.0) * cw - twoSqrtAalpha);
                a0 = (a + 1.0) - (a - 1.0) * cw + twoSqrtAalpha;
                a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cw);
                a2 = (a + 1.0) - (a - 1.0) * cw - twoSqrtAalpha;
            }
        }

        for (int chIdx = 0; chIdx < numChannels; ++chIdx)
        {
            auto& s = sections[chIdx][section];
            s.b0 = (float) (b0 / a0);
            s.b1 = (float) (b1 / a0);
            s.b2 = (float) (b2 / a0);
            s.a1 = (float) (a1 / a0);
            s.a2 = (float) (a2 / a0);
        }
    }
}

void ToneShaper::process (juce::AudioBuffer<float>& buffer)
{
    const int numCh = juce::jmin (buffer.getNumChannels(), numChannels);
    const int n = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0 || ! params.enabled || params.tone == 0.0f)
        return;

    for (int chIdx = 0; chIdx < numCh; ++chIdx)
    {
        auto& low = sections[chIdx][0];
        auto& high = sections[chIdx][1];
        auto& pres = sections[chIdx][2];
        auto* dst = buffer.getWritePointer (chIdx);

        for (int i = 0; i < n; ++i)
        {
            const float x = dst[i];

            float y = low.b0 * x + low.b1 * low.x1 + low.b2 * low.x2
                    - low.a1 * low.y1 - low.a2 * low.y2;
            low.x2 = low.x1; low.x1 = x; low.y2 = low.y1; low.y1 = y;
            float v = y;

            y = high.b0 * v + high.b1 * high.x1 + high.b2 * high.x2
              - high.a1 * high.y1 - high.a2 * high.y2;
            high.x2 = high.x1; high.x1 = v; high.y2 = high.y1; high.y1 = y;
            v = y;

            y = pres.b0 * v + pres.b1 * pres.x1 + pres.b2 * pres.x2
              - pres.a1 * pres.y1 - pres.a2 * pres.y2;
            pres.x2 = pres.x1; pres.x1 = v; pres.y2 = pres.y1; pres.y1 = y;

            dst[i] = y;
        }
    }
}
