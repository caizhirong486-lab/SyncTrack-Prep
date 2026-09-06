// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DenoiseStage.h"

#include <memory>
#include <vector>

struct DFState;

extern "C"
{
    DFState* df_create (const char* path, float atten_lim, const char* log_level);
    void df_free (DFState* st);
    size_t df_get_frame_length (DFState* st);
    float df_process_frame (DFState* st, float* input, float* output);
    void df_set_atten_lim (DFState* st, float lim_db);
}

/**
 * Live-tier engine: DeepFilterNet3 via the libDF C API (Rust/tract).
 *
 * One DFState per channel (libDF is mono), frame-driven FIFOs mirroring the
 * NoiseSuppressor STFT pattern so output is block-size independent. Runs
 * natively at 48 kHz; other session rates go through StageResampler.
 *
 * Amount -> attenuation limit: 0% = 0 dB (a real bypass, see process()),
 * 100% = 95 dB. The ceiling is 95 and not 100 on purpose: libDF switches to
 * its "unlimited" branch at >= 100 dB and segfaults inside tract on this
 * model (pitfall 2026-09-06). Linear in dB.
 */
class Dfn3Denoise : public DenoiseStage
{
public:
    /** Directory-independent model path: the DeepFilterNet3 tar.gz bundle that
        libDF loads (enc/erb_dec/df_dec onnx + config.ini). */
    void setModelPath (const juce::File& f) { modelFile = f; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override;
    void reset() override;
    void setAmount (float amount01) override;
    void process (juce::AudioBuffer<float>& buffer) override;
    int getLatencySamples() const override { return latencySamples; }
    bool supportsRealtime() const override { return true; }
    bool isLoaded() const { return ! states.empty() && states[0] != nullptr; }

private:
    /** libDF attenuation limit for the current Amount; capped below libDF's
        unlimited branch (>= 100 dB crashes tract on this model). */
    float attenLimDb() const { return maxAttenLimDb * amount01; }
    static constexpr float maxAttenLimDb = 95.0f;

    int probeLatency48();

    juce::File modelFile;
    std::vector<DFState*> states;
    StageResampler resampler;
    bool resample = false;
    double sessionRate = 48000.0;
    double engineRate = 48000.0;

    int frameLen = 480;
    int latencySamples = 0;
    int latency48Cache = 0; // measured model delay in 48k-domain samples
    float amount01 = 0.45f;

    // 48k-domain FIFOs (one per channel)
    std::vector<std::vector<float>> inFifo, outFifo;
    // amount==0 bypass: frame history that preserves the model's content delay
    std::vector<std::vector<float>> bypassQueue;
    std::vector<int> bypassQueued;
    std::vector<int> fifoWrite, hopCounter, outRead, outAvail;
    std::vector<float> frameIn, frameOut;
    std::vector<float> resampleIn, resampleOut; // RT-safe scratch, sized in prepare
    std::vector<std::vector<float>> frameCarry;  // partial 48k frame per channel
    std::vector<int> frameCarried;
    int maxBlock48 = 512;
};
