// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DenoiseStage.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

/**
 * HQ runtime lifecycle, surfaced to the editor and the tests. The short
 * window (realtime + Short/Mixdown offline) and the 4 s DOP window share one
 * process-wide MaskNet session.
 */
enum class HqRuntimeState
{
    inactive,          // HQ not selected or nothing loaded yet
    loading,           // background loader is reading the 228 MB model
    warming,           // session loaded, first windows still priming
    shortActive,       // 160 ms short window engaged (realtime or offline)
    dop4sOffline,      // 4 s DOP window engaged (offline only)
    fallbackCapacity,  // another instance holds the realtime HQ lease
    fallbackDeadline,  // steady-state window missed its deadline (this generation)
    fallbackModelError // model/session unavailable: permanent fallback for this render
};

/** Process-wide dynamic MaskNet session with a background loader.

    The loader creates the ORT session and runs both steady-state shapes
    (batch 2 x 16 frames and batch 2 x 496 frames) as a warm-up so the first
    real window never pays first-call costs. prepareToPlay never blocks on
    the loader; only offline renders wait (max 30 s, per plan). */
class MossFormerMaskNet
{
public:
    enum class LoadState { idle, loading, warming, ready, failed };

    static MossFormerMaskNet& instance();

    /** Idempotent: starts the loader thread once for the given model path. */
    void requestLoad (const juce::File& modelFile);

    LoadState loadState() const { return loadState_.load (std::memory_order_acquire); }
    juce::String error() const
    {
        const std::lock_guard<std::mutex> lock (errorMutex);
        return errorMessage;
    }
    /** Blocks up to timeoutMs (offline render start only). */
    bool waitReady (int timeoutMs);
    void resetForTests();

    /** [batch, frames, 180] -> [batch, frames, 961] (frame-major rows).
        False on inference failure; caller must not retry the same window. */
    bool run (const float* feats, int batch, int frames, std::vector<float>& maskOut);

private:
    MossFormerMaskNet() = default;
    ~MossFormerMaskNet();
    void loaderLoop (juce::File modelFile);

    std::atomic<LoadState> loadState_ { LoadState::idle };
    mutable std::mutex errorMutex;
    juce::String errorMessage;
    std::thread loader;
    bool loaderStarted = false;

    struct Session;
    std::unique_ptr<Session> session;

    JUCE_DECLARE_NON_COPYABLE (MossFormerMaskNet)
};
