// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DenoiseStage.h"
#include "MossFormerFrontend.h"
#include "MossFormerMaskNet.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class Dfn3Denoise;

/**
 * HQ short-window engine: 160 ms window, 120 ms stride, 20 ms edge trims at
 * the fixed 250 ms total plugin latency (12000 samples @ 48 kHz engine
 * domain). One window = 16 fbank frames; both channels run through the
 * shared dynamic MaskNet as batch 2 (bit-identical to per-channel batch 1).
 *
 * Realtime: the audio thread only feeds preallocated SPSC rings, drives the
 * parallel DFN3 alignment chain and mixes the worker output; a single
 * background worker performs frontend -> inference -> backend. The audio
 * thread never calls ORT, waits, locks or allocates.
 *
 * Offline (setSynchronous): windows run inline on the host render thread;
 * before the first inference the loader may be awaited up to 30 s, then the
 * whole render locks to one engine (HQ or aligned DFN3 fallback).
 *
 * The DFN3 alignment chain (realtime) is padded to the HQ 250 ms contract so
 * priming, seeks and deadline-miss degradation swap engines through the 15 ms
 * equal-power crossfade without changing the reported latency.
 */
class MossFormerShortDenoise : public DenoiseStage
{
public:
    MossFormerShortDenoise() = default;
    ~MossFormerShortDenoise() override;

    void setModelPath (const juce::File& f) { modelFile = f; }
    void setMelPath (const juce::File& f) { melFile = f; }
    /** Processor-owned DFN3 engine shared with the Live tier (the two are
        never active simultaneously as the rendered stage). */
    void attachDfn3 (Dfn3Denoise* d) { dfn3 = d; }

    void prepare (double sampleRate, int maxBlock, int numChannels) override;
    void reset() override;
    void setAmount (float amount01) override;
    void process (juce::AudioBuffer<float>& buffer) override;
    int getLatencySamples() const override { return latencySamples; }
    bool supportsRealtime() const override { return true; }

    /** Offline renders run the windows inline on the calling thread. */
    void setSynchronous (bool sync) { synchronous.store (sync, std::memory_order_relaxed); }

    /** New transport discontinuity (audio thread): bumps the generation, the
        worker drops its queue, output re-primes on the aligned DFN3 chain.
        `sessionStart` is the first session-domain sample of the new stream. */
    void bumpGeneration (std::int64_t sessionStart);

    HqRuntimeState runtimeState() const;

    /** Set once the single-instance lease is lost: permanently serve the
        aligned DFN3 chain without starting the worker pipeline. */
    void setCapacityFallback (bool on) { capacityFallback.store (on, std::memory_order_relaxed); }

    /** Test hook: count the next steady-state window as a deadline miss. */
    void forceDeadlineMiss() { forceMiss.store (true, std::memory_order_relaxed); }

    /** Test hooks (diagnostics only). */
    int deadlineMissesForTest() const { return deadlineMisses.load(); }
    HqRuntimeState debugStateForTest() const { return state.load(); }
    std::int64_t debugRingBacklog() const
    {
        return hqOut48[0].written() - hqOut48[0].r.load (std::memory_order_relaxed);
    }

    static constexpr int win48 = 7680;      // 160 ms
    static constexpr int stride48 = 5760;   // 120 ms = 15 fbank hops
    static constexpr int trim48 = 960;      // 20 ms
    static constexpr int framesPerWin = (win48 - MossFormerFrontend::kWinLen)
                                         / MossFormerFrontend::kHop + 1; // 16
    static constexpr int contract48 = 12000; // 250 ms at 48 kHz
    static constexpr int fade48 = 720;       // 15 ms equal-power crossfade (48k)

private:
    /** Classic SPSC ring: the writer owns `w`, the reader owns `r`. */
    struct Ring
    {
        std::vector<float> buf;
        std::atomic<std::int64_t> w { 0 }, r { 0 };
        void init (std::size_t n) { buf.assign (n, 0.0f); w.store (0); r.store (0); }
        std::int64_t written() const { return w.load (std::memory_order_acquire); }
        void push (const float* src, int n);
        void pop (float* dst, int n);
    };

    // worker side
    void workerLoop();
    bool runWindow (int winK, std::uint64_t frameBase, std::int64_t epochStartW,
                    int pad0, std::int64_t& readCursor);
    // offline inline path

    juce::File modelFile, melFile;
    Dfn3Denoise* dfn3 = nullptr;
    StageResampler resampler, hqResampler;
    bool resample = false;
    double sessionRate = 48000.0, engineRate = 48000.0;
    int latencySamples = 0;
    int numCh = 2;
    int maxBlockSession = 512;

    std::shared_ptr<const MossFormerFrontend::Constants> constants;
    std::shared_ptr<const std::vector<float>> dopDither; // unused here; loaded once process-wide

    std::atomic<bool> synchronous { false };
    std::atomic<bool> capacityFallback { false };
    std::atomic<bool> forceMiss { false };
    std::atomic<int> generation { 0 };
    std::atomic<std::uint64_t> streamEpoch { 0 };
    std::atomic<std::int64_t> alignBase48 { 0 };
    std::atomic<int64_t> pad0Samples { 0 };
    std::atomic<float> amount01 { 0.45f };
    std::atomic<HqRuntimeState> state { HqRuntimeState::inactive };
    std::atomic<int> deadlineMisses { 0 };

    // audio-thread world
    std::vector<float> inputCopy, dfnAligned; // dfnAligned: numCh * maxBlock
    std::vector<float> dfnDelay;              // per-channel alignment delay ring
    int dfnDelayLen = 0, dfnDelayCap = 0, dfnDelayPos = 0;
    std::vector<float> hqSession;             // per-channel session-domain HQ ring
    std::int64_t hqSessionCap = 0;
    std::array<std::int64_t, 2> hqWritePos { 0, 0 };
    std::int64_t hqReadPos = 0;
    std::vector<float> tmp48, drain48, fadeMix;
    int fadeLen = 0;
    int fadePos = 0, fadeOutPos = -1;
    bool hqEngaged = false;
    bool offlineHq = false, offlineFallback = false;
    std::int64_t served = 0;
    std::int64_t syncWinK = 0;
    std::array<Ring, 2> hqOut48;              // worker -> audio (48k domain)
    std::array<std::vector<float>, 2> syncIn; // offline input accumulation (48k)
    std::int64_t syncWritten = 0;
    std::array<std::int64_t, 2> syncWrittenCh { 0, 0 };

    void appendHqSession (int ch, const float* src, int n);
    void assembleOutput (juce::AudioBuffer<float>& buffer, int n, bool useA);

    // worker world
    std::thread worker;
    std::mutex workerMutex;
    std::condition_variable workerCv;
    std::atomic<bool> workerRunning { false };
    std::array<Ring, 2> in48;                // audio -> worker (48k domain)

    MossFormerFrontend::Scratch scratch;
    std::array<std::vector<float>, 2> winBuf, wetBuf;
    std::vector<float> ditheredWin, feats2, mask;

    JUCE_DECLARE_NON_COPYABLE (MossFormerShortDenoise)
};
