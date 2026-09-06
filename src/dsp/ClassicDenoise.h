// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DenoiseStage.h"
#include "NoiseSuppressor.h"

/**
 * Adapts the existing spectral NoiseSuppressor to the DenoiseStage interface.
 * Bit-identical to running the NoiseSuppressor directly with the same params
 * (pinned by test_classic_denoise_regression.cpp).
 */
class ClassicDenoise : public DenoiseStage
{
public:
    void prepare (double sampleRate, int maxBlock, int numChannels) override
    {
        ns.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, maxBlock),
                      (juce::uint32) juce::jmax (1, numChannels) });
    }

    void reset() override { ns.reset(); }

    void setAmount (float amount01) override
    {
        params.amount = juce::jlimit (0.0f, 1.0f, amount01);
        ns.setParams (params);
    }

    /** Full preset-driven parameter set (the processor pushes these on every
        updateDspParams, then applies the Amount knob on top). */
    void setClassicParams (const NoiseSuppressor::Params& p)
    {
        params = p;
        ns.setParams (params);
    }

    void process (juce::AudioBuffer<float>& buffer) override { ns.process (buffer); }

    int getLatencySamples() const override { return ns.getLatencySamples(); }
    bool supportsRealtime() const override { return true; }

    NoiseSuppressor& inner() { return ns; }

private:
    NoiseSuppressor ns;
    NoiseSuppressor::Params params;
};
