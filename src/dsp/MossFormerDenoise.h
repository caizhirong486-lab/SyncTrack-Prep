// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DenoiseStage.h"

#include <memory>
#include <vector>

namespace Ort
{
struct Env; // fwd: real header provided by onnxruntime
}

/**
 * HQ-tier engine: MossFormer2_SE_48K via ONNX Runtime.
 *
 * Non-realtime only (supportsRealtime() == false): the streaming scheme is
 * fixed-window offline processing — 4 s window, 3 s stride, 0.5 s discarded
 * from each output edge (identical to the Python reference stitching), with a
 * 0.5 s leading zero pad. Chunks are emitted deterministically at stride
 * boundaries, so the content delay is exactly 3.5 s at 48 kHz plus any
 * resampler latency; no jitter, no mid-stream padding after the priming run.
 *
 * Amount = wet/dry balance (0% = dry, 100% = fully denoised).
 */
class MossFormerDenoise : public DenoiseStage
{
public:
    MossFormerDenoise(); // out-of-line with the dtor: exception cleanup needs complete Session
    void setModelPath (const juce::File& f) { modelFile = f; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override;
    ~MossFormerDenoise() override; // out-of-line: unique_ptr<Session> needs the complete type
    void reset() override;
    void setAmount (float amount01) override;
    void process (juce::AudioBuffer<float>& buffer) override;
    int getLatencySamples() const override { return latencySamples; }
    bool supportsRealtime() const override { return false; }
    bool isLoaded() const { return session != nullptr; }

    /** 4 s window, 3 s stride at 48 kHz — identical to the ClearerVoice
        reference decode (no leading pad; the first chunk discards only its
        tail, later chunks discard 0.5 s from each edge). */
    static constexpr int pad48 = 0;
    static constexpr int window48 = 192000;
    static constexpr int stride48 = 144000;
    static constexpr int trim48 = 24000;
    /** Content delay at 48 kHz: the first chunk covers input [0, 3.5 s) and
        becomes servable when its window completes at input 4 s. */
    static constexpr int latency48 = window48;

private:
    /** Runs one window across all channels; false = window not run (queue
        short) or inference failed, and the caller must not retry it. */
    bool runWindow (int w);
    bool allChannelsReady() const;

    juce::File modelFile;
    StageResampler resampler;
    bool resample = false;
    double sessionRate = 48000.0;
    double engineRate = 48000.0;
    int latencySamples = 0;
    float amount01 = 0.45f;
    int numCh = 2;
    bool active = false; // inference engaged (model loaded)

    // 48k-domain per-channel input queues (pad included) and output stream
    std::vector<std::vector<float>> in48, out48, sessionQueue;
    std::vector<int> servedSession;
    int windowsRun = 0;

    struct Session;
    std::unique_ptr<Session> session;
};
