// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MossFormerShortDenoise.h"

#include "Dfn3Denoise.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifdef STP_ENABLE_MOSSFORMER

namespace
{
constexpr int kHop = MossFormerFrontend::kHop;
constexpr int kMissSlack48 = 2400; // 50 ms of tolerated window-completion lag

float fadeGainOld (float t) // equal-power curve: 1 = outgoing, 0 = incoming
{
    return 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * t));
}
} // namespace

void MossFormerShortDenoise::Ring::push (const float* src, int n)
{
    const std::int64_t cap = (std::int64_t) buf.size();
    const std::int64_t w0 = w.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        buf[(std::size_t) ((w0 + i) % cap)] = src[i];
    w.store (w0 + n, std::memory_order_release);
}

void MossFormerShortDenoise::Ring::pop (float* dst, int n)
{
    const std::int64_t cap = (std::int64_t) buf.size();
    const std::int64_t r0 = r.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        dst[i] = buf[(std::size_t) ((r0 + i) % cap)];
    r.store (r0 + n, std::memory_order_release);
}

MossFormerShortDenoise::~MossFormerShortDenoise()
{
    workerRunning = false;
    workerCv.notify_all();
    if (worker.joinable())
        worker.join();
}

void MossFormerShortDenoise::prepare (double sampleRate, int maxBlock, int numChannels)
{
    sessionRate = sampleRate;
    engineRate = 48000.0;
    numCh = juce::jmax (1, numChannels);
    maxBlockSession = juce::jmax (1, maxBlock);

    juce::String err;
    constants = MossFormerFrontend::loadConstants (melFile, err);
    MossFormerMaskNet::instance().requestLoad (modelFile);

    resampler.prepare (sessionRate, numCh, maxBlockSession);
    resample = resampler.isActive();
    hqResampler.prepare (sessionRate, numCh, maxBlockSession);

    const int block48 = (int) std::ceil ((double) maxBlockSession
                                         / juce::jmax (0.01, sessionRate / engineRate)) + 64;
    latencySamples = (int) std::ceil (contract48 * (sessionRate / engineRate)
                                      + resampler.latencySamples());
    fadeLen = (int) std::ceil ((double) fade48 * (sessionRate / engineRate));

    const std::size_t ringCap = (std::size_t) juce::jmax (win48 * 4, block48 * 16);
    for (auto& r : in48)
        r.init (ringCap);
    for (auto& r : hqOut48)
        r.init (ringCap);

    inputCopy.assign ((std::size_t) numCh * (std::size_t) maxBlockSession, 0.0f);
    dfnAligned.assign ((std::size_t) numCh * (std::size_t) maxBlockSession, 0.0f);
    tmp48.assign ((std::size_t) block48 + 64, 0.0f);
    drain48.assign ((std::size_t) win48 * 2, 0.0f);

    // Realtime: the ring covers several windows of burst. Offline: the render
    // thread outpaces the latency-gated reader for the whole flush, so
    // appendHqSession grows the buffer instead (single-threaded world).
    hqSessionCap = (std::int64_t) juce::jmax (win48 * 4, maxBlockSession * 16);
    hqSession.assign ((std::size_t) numCh * (std::size_t) hqSessionCap, 0.0f);
    hqWritePos = { 0, 0 };
    hqReadPos = 0;

    // DFN3 alignment delay = HQ contract − DFN3's own content delay. The
    // ring IS the delay: one slot per delayed sample, per channel.
    const int dfnLat = dfn3 != nullptr ? dfn3->getLatencySamples() : 0;
    dfnDelayLen = juce::jmax (0, latencySamples - dfnLat);
    dfnDelayCap = juce::jmax (1, dfnDelayLen);
    dfnDelay.assign ((std::size_t) numCh * (std::size_t) dfnDelayCap, 0.0f);
    dfnDelayPos = 0;

    fadeMix.resize ((std::size_t) juce::jmax (1, fadeLen));
    for (int i = 0; i < fadeLen; ++i)
        fadeMix[(std::size_t) i] = fadeGainOld ((float) i / (float) fadeLen);

    for (auto& w : winBuf)
        w.assign (win48, 0.0f);
    for (auto& w : wetBuf)
        w.assign (win48, 0.0f);
    ditheredWin.assign (win48, 0.0f);

    workerRunning = true;
    if (! worker.joinable() && ! synchronous.load (std::memory_order_relaxed))
        worker = std::thread ([this] { workerLoop(); });

    state.store (constants == nullptr ? HqRuntimeState::fallbackModelError
                                      : HqRuntimeState::loading,
                 std::memory_order_relaxed);
    reset();
}

void MossFormerShortDenoise::reset()
{
    served = 0;
    fadePos = 0;
    fadeOutPos = -1;
    hqEngaged = false;
    offlineHq = false;
    offlineFallback = false;
    syncWritten = 0;
    syncWrittenCh = { 0, 0 };
    syncWinK = 0;
    hqWritePos = { 0, 0 };
    hqReadPos = 0;
    deadlineMisses.store (0, std::memory_order_relaxed);
    std::fill (dfnDelay.begin(), dfnDelay.end(), 0.0f);
    dfnDelayPos = 0;
    std::fill (hqSession.begin(), hqSession.end(), 0.0f);
    for (auto& r : hqOut48)
        r.r.store (r.w.load (std::memory_order_acquire), std::memory_order_release);
    for (auto& r : in48)
        r.r.store (r.w.load (std::memory_order_acquire), std::memory_order_release);
    for (auto& s : syncIn)
    {
        s.clear();
        s.resize ((std::size_t) win48 + (std::size_t) maxBlockSession * 4 + 64, 0.0f);
    }
    alignBase48.store (0, std::memory_order_relaxed);
    pad0Samples.store (0, std::memory_order_relaxed);
    resampler.reset();
    hqResampler.reset();
    if (dfn3 != nullptr)
        dfn3->reset();
}

void MossFormerShortDenoise::bumpGeneration (std::int64_t sessionStart)
{
    generation.fetch_add (1, std::memory_order_release);
    const std::int64_t start48 = (std::int64_t) std::llround (
        (double) sessionStart * (engineRate / sessionRate));
    alignBase48.store ((start48 / kHop) * kHop, std::memory_order_relaxed);
    pad0Samples.store (start48 - (start48 / kHop) * kHop, std::memory_order_relaxed);
    streamEpoch.fetch_add (1, std::memory_order_release);
    served = 0;
    fadePos = 0;
    fadeOutPos = -1;
    hqEngaged = false;
    syncWinK = 0;
    syncWritten = 0;
    syncWrittenCh = { 0, 0 };
    hqWritePos = { 0, 0 };
    hqReadPos = 0;
    std::fill (dfnDelay.begin(), dfnDelay.end(), 0.0f);
    dfnDelayPos = 0;
    std::fill (hqSession.begin(), hqSession.end(), 0.0f);
    for (auto& r : hqOut48)
        r.r.store (r.w.load (std::memory_order_acquire), std::memory_order_release);
    resampler.reset();
    hqResampler.reset();
    if (dfn3 != nullptr)
        dfn3->reset();
    workerCv.notify_all();
}

void MossFormerShortDenoise::setAmount (float amount01In)
{
    amount01.store (juce::jlimit (0.0f, 1.0f, amount01In), std::memory_order_relaxed);
    if (dfn3 != nullptr)
        dfn3->setAmount (amount01In);
}

HqRuntimeState MossFormerShortDenoise::runtimeState() const
{
    if (capacityFallback.load (std::memory_order_relaxed))
        return HqRuntimeState::fallbackCapacity;
    return state.load (std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// worker thread
// ---------------------------------------------------------------------------

bool MossFormerShortDenoise::runWindow (int winK, std::uint64_t frameBase,
                                        std::int64_t epochStartW, int pad0,
                                        std::int64_t& readCursor)
{
    // Assemble both channels; window 0 zero-fills up to the fbank grid and
    // never emits its pre-generation head.
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& w = winBuf[(std::size_t) ch];
        const auto& ring = in48[(std::size_t) ch];
        const std::int64_t cap = (std::int64_t) ring.buf.size();
        int filled = 0;
        if (winK == 0 && pad0 > 0)
        {
            std::fill (w.begin(), w.begin() + pad0, 0.0f);
            filled = pad0;
        }
        for (; filled < win48; ++filled)
            w[(std::size_t) filled] =
                ring.buf[(std::size_t) ((epochStartW + readCursor + filled
                                         - (winK == 0 ? pad0 : 0)) % cap)];
    }
    readCursor += win48 - (winK == 0 ? pad0 : 0);

    if (! MossFormerFrontend::buildFeatsAndRun (*constants, scratch, winBuf,
                                                framesPerWin,
                                                frameBase + (std::uint64_t) (winK * 15),
                                                MossFormerFrontend::DitherMode::shortHash,
                                                nullptr, feats2, mask))
        return false;

    MossFormerFrontend::renderWet (*constants, scratch, winBuf, mask, wetBuf,
                                   amount01.load (std::memory_order_relaxed),
                                   framesPerWin, win48, trim48,
                                   winK == 0 ? 0 : trim48);
    const int emitLen = win48 - trim48 - (winK == 0 ? 0 : trim48);
    int skip = 0;
    if (winK == 0 && pad0 > 0)
        skip = pad0; // pre-generation head is never emitted
    for (int ch = 0; ch < 2; ++ch)
        hqOut48[(std::size_t) ch].push (wetBuf[(std::size_t) ch].data() + skip,
                                        emitLen - skip);
    return true;
}

void MossFormerShortDenoise::workerLoop()
{
    std::uint64_t seenEpoch = streamEpoch.load (std::memory_order_acquire);
    std::int64_t epochStartW = in48[0].written();
    std::int64_t readCursor = 0;
    int winK = 0;
    bool stopThisGeneration = false;

    while (workerRunning)
    {
        const std::uint64_t epoch = streamEpoch.load (std::memory_order_acquire);
        if (epoch != seenEpoch)
        {
            seenEpoch = epoch;
            epochStartW = in48[0].written();
            readCursor = 0;
            winK = 0;
            stopThisGeneration = false;
            for (auto& r : in48)
                r.r.store (r.w.load (std::memory_order_acquire), std::memory_order_release);
        }

        if (capacityFallback.load (std::memory_order_relaxed) || stopThisGeneration)
        {
            std::unique_lock<std::mutex> lock (workerMutex);
            workerCv.wait_for (lock, std::chrono::milliseconds (50));
            continue;
        }

        const int pad0 = (int) pad0Samples.load (std::memory_order_relaxed);
        const std::int64_t need = (std::int64_t) winK * stride48 + win48 - pad0;
        if (in48[0].written() - epochStartW < need)
        {
            std::unique_lock<std::mutex> lock (workerMutex);
            workerCv.wait_for (lock, std::chrono::milliseconds (20));
            continue;
        }

        if (forceMiss.exchange (false, std::memory_order_relaxed) && winK >= 2)
        {
            stopThisGeneration = true;
            deadlineMisses.fetch_add (1, std::memory_order_relaxed);
            state.store (HqRuntimeState::fallbackDeadline, std::memory_order_relaxed);
            continue;
        }

        const std::uint64_t base = (std::uint64_t) alignBase48.load (std::memory_order_relaxed)
                                   / (std::uint64_t) kHop;
        if (! runWindow (winK, base, epochStartW, pad0, readCursor))
        {
            stopThisGeneration = true;
            state.store (HqRuntimeState::fallbackModelError, std::memory_order_relaxed);
            continue;
        }
        ++winK;

        // Deadline: completing window k must leave window k+1's input ready.
        if (winK >= 3)
        {
            const std::int64_t needNext = (std::int64_t) winK * stride48 + win48;
            if (in48[0].written() - epochStartW < needNext - kMissSlack48)
            {
                stopThisGeneration = true;
                deadlineMisses.fetch_add (1, std::memory_order_relaxed);
                state.store (HqRuntimeState::fallbackDeadline, std::memory_order_relaxed);
            }
        }
        if (state.load (std::memory_order_relaxed) == HqRuntimeState::loading
            || state.load (std::memory_order_relaxed) == HqRuntimeState::inactive)
            state.store (HqRuntimeState::warming, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// audio thread (realtime) / render thread (offline)
// ---------------------------------------------------------------------------

void MossFormerShortDenoise::appendHqSession (int ch, const float* src, int n)
{
    if (synchronous.load (std::memory_order_relaxed))
    {
        // Offline: linear layout with growth (single-threaded render world).
        const auto need = hqWritePos[(std::size_t) ch] + n;
        if (need > hqSessionCap)
        {
            const auto newCap = juce::jmax (hqSessionCap * 2, need + win48);
            std::vector<float> bigger ((std::size_t) numCh * (std::size_t) newCap, 0.0f);
            for (int c2 = 0; c2 < numCh; ++c2)
                std::memcpy (bigger.data() + (std::size_t) c2 * (std::size_t) newCap,
                             hqSession.data() + (std::size_t) c2 * (std::size_t) hqSessionCap,
                             (std::size_t) hqWritePos[(std::size_t) c2] * sizeof (float));
            hqSession.swap (bigger);
            hqSessionCap = newCap;
        }
        std::memcpy (hqSession.data() + (std::size_t) ch * (std::size_t) hqSessionCap
                         + (std::size_t) hqWritePos[(std::size_t) ch],
                     src, (std::size_t) n * sizeof (float));
        hqWritePos[(std::size_t) ch] += n;
        return;
    }
    for (int i = 0; i < n; ++i)
        hqSession[(std::size_t) ch * (std::size_t) hqSessionCap
                  + (std::size_t) ((hqWritePos[(std::size_t) ch] + i) % hqSessionCap)] = src[i];
    hqWritePos[(std::size_t) ch] += n;
}

void MossFormerShortDenoise::process (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() <= 0)
        return;
    const bool sync = synchronous.load (std::memory_order_relaxed);

    if (constants == nullptr)
    {
        state.store (HqRuntimeState::fallbackModelError, std::memory_order_relaxed);
        return; // passthrough; the UI surfaces the model error
    }

    // capture the dry input (normalised domain, per channel)
    for (int ch = 0; ch < numCh; ++ch)
    {
        const int src = juce::jmin (ch, buffer.getNumChannels() - 1);
        std::memcpy (inputCopy.data() + (std::size_t) ch * (std::size_t) n,
                     buffer.getReadPointer (src), (std::size_t) n * sizeof (float));
    }

    // Offline renders lock to one engine before the first inference.
    bool useA = false;
    if (sync)
    {
        if (! offlineHq && ! offlineFallback)
        {
            const bool ready = MossFormerMaskNet::instance().waitReady (30000);
            offlineHq = ready && ! capacityFallback.load (std::memory_order_relaxed);
            offlineFallback = ! offlineHq;
        }
        useA = offlineFallback;
    }
    else
    {
        useA = true; // realtime: the aligned DFN3 chain always runs
        if (capacityFallback.load (std::memory_order_relaxed))
            state.store (HqRuntimeState::fallbackCapacity, std::memory_order_relaxed);
    }

    if (sync)
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            resampler.upsample (inputCopy.data() + (std::size_t) ch * (std::size_t) n, n, ch);
            while (true)
            {
                const int got = resampler.take48 (tmp48.data(), (int) tmp48.size(), ch);
                if (got <= 0)
                    break;
                auto& acc = syncIn[(std::size_t) ch];
                if ((std::int64_t) acc.size() < syncWrittenCh[(std::size_t) ch] + got + win48)
                    acc.resize ((std::size_t) syncWrittenCh[(std::size_t) ch]
                                + (std::size_t) got + win48);
                std::memcpy (acc.data() + (std::size_t) syncWrittenCh[(std::size_t) ch],
                             tmp48.data(), (std::size_t) got * sizeof (float));
                syncWrittenCh[(std::size_t) ch] += got;
            }
        }
        syncWritten = juce::jmin (syncWrittenCh[0], syncWrittenCh[1]);

        if (offlineHq)
        {
            while (true)
            {
                const std::int64_t need = syncWinK * stride48 + win48;
                if (syncWritten < need)
                    break;
                for (int ch = 0; ch < 2; ++ch)
                    std::memcpy (winBuf[(std::size_t) ch].data(),
                                 syncIn[(std::size_t) ch].data()
                                     + (std::size_t) (syncWinK * stride48),
                                 (std::size_t) win48 * sizeof (float));
                if (! MossFormerFrontend::buildFeatsAndRun (*constants, scratch, winBuf,
                                                            framesPerWin,
                                                            (std::uint64_t) (syncWinK * 15),
                                                            MossFormerFrontend::DitherMode::shortHash,
                                                            nullptr, feats2, mask))
                {
                    offlineHq = false;
                    offlineFallback = true;
                    state.store (HqRuntimeState::fallbackModelError, std::memory_order_relaxed);
                    break;
                }
                // renderWet memmoves the emitted span to [0..): append it whole
                MossFormerFrontend::renderWet (*constants, scratch, winBuf, mask, wetBuf,
                                               amount01.load (std::memory_order_relaxed),
                                               framesPerWin, win48, trim48,
                                               syncWinK == 0 ? 0 : trim48);
                const int emitLen = win48 - trim48 - (syncWinK == 0 ? 0 : trim48);
                for (int ch = 0; ch < 2; ++ch)
                    appendHqSession (ch, wetBuf[(std::size_t) ch].data(), emitLen);
                ++syncWinK;
                state.store (HqRuntimeState::shortActive, std::memory_order_relaxed);
            }
        }
    }
    else
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            resampler.upsample (inputCopy.data() + (std::size_t) ch * (std::size_t) n, n, ch);
            while (true)
            {
                const int got = resampler.take48 (tmp48.data(), (int) tmp48.size(), ch);
                if (got <= 0)
                    break;
                in48[(std::size_t) ch].push (tmp48.data(), got);
            }
        }
        workerCv.notify_all();

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& ring = hqOut48[(std::size_t) ch];
            while (ring.written() - ring.r.load (std::memory_order_relaxed) > 0)
            {
                const std::int64_t avail = ring.written()
                                           - ring.r.load (std::memory_order_relaxed);
                const int take = (int) juce::jmin<std::int64_t> (avail, (std::int64_t) drain48.size());
                ring.pop (drain48.data(), take);
                hqResampler.push48 (drain48.data(), take, ch);
            }
            while (true)
            {
                const int got = hqResampler.downsample (tmp48.data(), (int) tmp48.size(), ch);
                if (got <= 0)
                    break;
                appendHqSession (ch, tmp48.data(), got);
            }
        }
        if (hqWritePos[0] > 0 && ! capacityFallback.load (std::memory_order_relaxed))
            state.store (HqRuntimeState::shortActive, std::memory_order_relaxed);
    }

    // Chain A runs AFTER the HQ feed: dfn3->process is in-place and must not
    // contaminate the dry input the HQ chain is still reading.
    if (useA && dfn3 != nullptr)
    {
        float* viewPtrs[2] = { inputCopy.data(),
                               numCh > 1 ? inputCopy.data() + n : inputCopy.data() };
        juce::AudioBuffer<float> view (viewPtrs, numCh, n);
        dfn3->process (view);
        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float delayed = dfnDelay[(std::size_t) ch * (std::size_t) dfnDelayCap
                                               + (std::size_t) dfnDelayPos];
                dfnDelay[(std::size_t) ch * (std::size_t) dfnDelayCap + (std::size_t) dfnDelayPos]
                    = inputCopy[(std::size_t) ch * (std::size_t) n + (std::size_t) i];
                dfnAligned[(std::size_t) ch * (std::size_t) n + (std::size_t) i] = delayed;
            }
            dfnDelayPos = (dfnDelayPos + 1) % dfnDelayCap;
        }
    }

    assembleOutput (buffer, n, useA);
    served += n;
}

void MossFormerShortDenoise::assembleOutput (juce::AudioBuffer<float>& buffer, int n, bool useA)
{
    const int chs = juce::jmin (numCh, buffer.getNumChannels());
    const std::int64_t bStart = latencySamples; // first HQ sample's output position
    const bool linear = synchronous.load (std::memory_order_relaxed);

    for (int i = 0; i < n; ++i)
    {
        const std::int64_t pos = served + i;
        const bool bAvail = pos >= bStart
                            && juce::jmin (hqWritePos[0], hqWritePos[1]) - hqReadPos > 0;
        if (bAvail && ! hqEngaged)
        {
            hqEngaged = true;
            fadePos = useA ? 0 : fadeLen;
            fadeOutPos = -1;
        }
        if (hqEngaged && ! bAvail && fadeOutPos < 0)
            fadeOutPos = 0; // HQ dried up: fade back onto the aligned chain

        float gNew = 0.0f; // weight of the HQ stream
        float b[2] = { 0.0f, 0.0f };
        bool haveB = false;

        if (hqEngaged && bAvail)
        {
            for (int ch = 0; ch < chs; ++ch)
                b[ch] = hqSession[(std::size_t) ch * (std::size_t) hqSessionCap
                                  + (std::size_t) (linear ? hqReadPos
                                                          : hqReadPos % hqSessionCap)];
            haveB = true;
            if (fadeOutPos >= 0)
            {
                gNew = fadeMix[(std::size_t) juce::jmin (fadeOutPos, fadeLen - 1)];
                ++fadeOutPos;
            }
            else
            {
                gNew = useA ? 1.0f - fadeMix[(std::size_t) juce::jmin (fadePos, fadeLen - 1)]
                            : 1.0f;
                if (fadePos < fadeLen)
                    ++fadePos;
            }
        }
        else if (hqEngaged && fadeOutPos >= 0)
        {
            gNew = fadeMix[(std::size_t) juce::jmin (fadeOutPos, fadeLen - 1)];
            ++fadeOutPos;
        }

        for (int ch = 0; ch < chs; ++ch)
        {
            const float a = useA ? dfnAligned[(std::size_t) ch * (std::size_t) n + (std::size_t) i]
                                 : 0.0f;
            float out;
            if (haveB)
                out = useA ? (1.0f - gNew) * a + gNew * b[ch] : b[ch];
            else
                out = useA ? (1.0f - gNew) * a : 0.0f;
            buffer.getWritePointer (ch)[i] = out;
        }

        if (haveB)
            ++hqReadPos;
        if (fadeOutPos >= fadeLen)
        {
            fadeOutPos = -1;
            hqEngaged = false;
        }
    }
}

#else // !STP_ENABLE_MOSSFORMER

MossFormerShortDenoise::~MossFormerShortDenoise() = default;
void MossFormerShortDenoise::prepare (double, int, int) {}
void MossFormerShortDenoise::reset() {}
void MossFormerShortDenoise::setAmount (float) {}
void MossFormerShortDenoise::process (juce::AudioBuffer<float>&) {}
void MossFormerShortDenoise::bumpGeneration (std::int64_t) {}

#endif
