// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DenoiseStage.h"
#include "MossFormerFrontend.h"
#include "MossFormerMaskNet.h"

class Dfn3Denoise;

#include <array>
#include <memory>
#include <vector>

/**
 * HQ-tier 4s DOP engine: MossFormer2_SE_48K MaskNet via the shared dynamic
 * ONNX session and the C++ ClearerVoice frontend/backend.
 *
 * Non-realtime only (supportsRealtime() == false): the streaming scheme is
 * fixed-window offline processing — 4 s window, 3 s stride, 0.5 s discarded
 * from each output edge (identical to the Python reference stitching), with
 * the first window keeping its left edge. DOP dither restarts every window
 * from the retired fixed graph's torch seed-20260906 table, so renders stay
 * aligned with the historical gold. Content delay is exactly 4 s at 48 kHz
 * plus resampler latency; no jitter, no mid-stream padding after priming.
 *
 * Offline renders lock the engine before the first inference (30 s loader
 * wait); a locked failure falls back to the DFN3 chain padded to the same
 * 4 s contract, and a mid-render ORT failure marks the file invalid.
 *
 * Amount = wet/dry balance (0% = dry, 100% = fully denoised).
 */
class MossFormerDenoise : public DenoiseStage
{
public:
    MossFormerDenoise(); // out-of-line with the dtor: exception cleanup needs complete Session
    void setModelPath (const juce::File& f) { modelFile = f; }
    void setMelPath (const juce::File& f) { melFile = f; }
    void setDopDitherPath (const juce::File& f) { dopDitherFile = f; }
    /** Processor-owned DFN3 engine for the offline fallback chain. */
    void attachDfn3 (Dfn3Denoise* d) { dfn3 = d; }
    /** Offline renders wait up to 30 s for the loader before the first block. */
    void setSyncWait (bool on) { syncWait = on; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override;
    ~MossFormerDenoise() override; // out-of-line: unique_ptr resources
    void reset() override;
    void setAmount (float amount01) override;
    void process (juce::AudioBuffer<float>& buffer) override;
    int getLatencySamples() const override { return latencySamples; }
    bool supportsRealtime() const override { return false; }
    bool isLoaded() const { return constants != nullptr && dopTable != nullptr; }

    HqRuntimeState runtimeState() const;

    /** Forensics (STP trace): window progress, queued input and priming state. */
    int debugWindowsRun() const { return windowsRun; }
    int debugServedSamples() const { return servedSession.empty() ? 0 : servedSession[0]; }
    int debugQueuedInput() const { return in48.empty() ? 0 : (int) in48[0].size(); }
    bool debugActive() const { return active; }
    /** Peak over the first `head` and the last `tail` samples of the channel-0
        input queue — distinguishes preroll silence from real material. */
    float debugQueueEdgePeak (int head, int tail) const
    {
        if (in48.empty()) return 0.0f;
        const auto& q = in48[0];
        float p = 0.0f;
        const int n = (int) q.size();
        for (int i = 0; i < juce::jmin (head, n); ++i)
            p = juce::jmax (p, std::abs (q[(size_t) i]));
        for (int i = juce::jmax (0, n - tail); i < n; ++i)
            p = juce::jmax (p, std::abs (q[(size_t) i]));
        return p;
    }

    /** 4 s window, 3 s stride at 48 kHz — identical to the ClearerVoice
        reference decode (no leading pad; the first chunk discards only its
        tail, later chunks discard 0.5 s from each edge). */
    static constexpr int pad48 = 0;
    static constexpr int window48 = 192000;
    static constexpr int stride48 = 144000;
    static constexpr int trim48 = 24000;
    static constexpr int framesPerDop = (window48 - MossFormerFrontend::kWinLen)
                                        / MossFormerFrontend::kHop + 1; // 496
    /** Content delay at 48 kHz: the first chunk covers input [0, 3.5 s) and
        becomes servable when its window completes at input 4 s. */
    static constexpr int latency48 = window48;

private:
    /** Runs one window across all channels; false = window not run (queue
        short) or inference failed, and the caller must not retry it. */
    bool runWindow (int w);
    bool allChannelsReady() const;
    void runFallback (juce::AudioBuffer<float>& buffer);

    juce::File modelFile, melFile, dopDitherFile;
    Dfn3Denoise* dfn3 = nullptr;
    StageResampler resampler;
    bool resample = false;
    double sessionRate = 48000.0;
    double engineRate = 48000.0;
    int latencySamples = 0;
    float amount01 = 0.45f;
    int numCh = 2;
    bool active = false;          // inference engaged (model + resources loaded)
    bool syncWait = false;        // offline: wait for the loader before block 1
    bool waitedForLoader = false;
    bool fallbackChain = false;   // locked to the padded DFN3 chain
    bool failedMidRender = false; // ORT failure after content: file is invalid

    std::shared_ptr<const MossFormerFrontend::Constants> constants;
    std::shared_ptr<const std::vector<float>> dopTable;
    MossFormerFrontend::Scratch scratch;
    std::array<std::vector<float>, 2> winBuf, wetBuf;
    std::vector<float> feats2, mask;
    std::vector<float> inputCopy, dfnDelay;
    int dfnDelayLen = 0, dfnDelayCap = 0, dfnDelayPos = 0;

    // 48k-domain per-channel input queues (pad included) and output stream
    std::vector<std::vector<float>> in48, out48, sessionQueue;
    std::vector<int> servedSession;
    int windowsRun = 0;

    JUCE_DECLARE_NON_COPYABLE (MossFormerDenoise)
};
