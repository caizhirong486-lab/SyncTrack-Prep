// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MossFormerDenoise.h"

#include "Dfn3Denoise.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef STP_ENABLE_MOSSFORMER

MossFormerDenoise::~MossFormerDenoise()
{
    MossFormerMaskNet::instance().finishLoad();
}
MossFormerDenoise::MossFormerDenoise() = default;

void MossFormerDenoise::prepare (double sampleRate, int maxBlock, int numChannels)
{
    sessionRate = sampleRate;
    numCh = juce::jmax (1, numChannels);
    resampler.prepare (sampleRate, numCh, juce::jmax (1, maxBlock));
    resample = resampler.isActive();
    engineRate = resample ? nnEngineSampleRate : sessionRate;

    juce::String err;
    constants = MossFormerFrontend::loadConstants (melFile, err);
    dopTable = MossFormerFrontend::loadDopDither (dopDitherFile, err);
    MossFormerMaskNet::instance().requestLoad (modelFile);
    if (constants == nullptr || dopTable == nullptr)
        active = false;

    // Offline fallback chain: DFN3 padded to the 4 s DOP contract.
    const int dfnLat = dfn3 != nullptr ? dfn3->getLatencySamples() : 0;
    const int contract = (int) std::ceil ((double) latency48 * (sessionRate / engineRate)
                                          + resampler.latencySamples());
    dfnDelayLen = juce::jmax (0, contract - dfnLat);
    dfnDelayCap = dfnDelayLen + juce::jmax (1, maxBlock);
    dfnDelay.assign ((size_t) numCh * (size_t) dfnDelayCap, 0.0f);
    dfnDelayPos = 0;
    inputCopy.assign ((size_t) numCh * (size_t) juce::jmax (1, maxBlock), 0.0f);

    const double sessionLatency = (double) latency48 * (sessionRate / engineRate)
                                  + resampler.latencySamples();
    latencySamples = (int) std::ceil (sessionLatency);

    for (auto& w : winBuf)
        w.assign (window48, 0.0f);
    for (auto& w : wetBuf)
        w.assign (window48, 0.0f);
    reset();
}

void MossFormerDenoise::reset()
{
    in48.assign ((size_t) numCh, {});
    out48.assign ((size_t) numCh, {});
    sessionQueue.assign ((size_t) numCh, {});
    servedSession.assign ((size_t) numCh, 0);
    windowsRun = 0;
    waitedForLoader = false;
    fallbackChain = false;
    failedMidRender = false;
    resampler.reset();
}

void MossFormerDenoise::setAmount (float amount01In)
{
    amount01 = juce::jlimit (0.0f, 1.0f, amount01In);
}

HqRuntimeState MossFormerDenoise::runtimeState() const
{
    if (failedMidRender || fallbackChain)
        return HqRuntimeState::fallbackModelError;
    if (! active)
        return constants == nullptr || dopTable == nullptr
                   ? HqRuntimeState::fallbackModelError
                   : HqRuntimeState::loading;
    return windowsRun > 0 ? HqRuntimeState::dop4sOffline : HqRuntimeState::warming;
}

bool MossFormerDenoise::runWindow (int w)
{
    const int emitOffset = w == 0 ? 0 : trim48;
    const int emitLen = window48 - trim48 - emitOffset;

    // Assemble both channel windows; the shared MaskNet takes batch 2.
    // Mono sessions duplicate channel 0 into the batch (bit-identical to a
    // per-channel pass, verified by the batch equivalence probe).
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& q = in48[(size_t) juce::jmin (ch, numCh - 1)];
        if ((int) q.size() < window48)
            return false;
        std::memcpy (winBuf[(size_t) ch].data(), q.data(), (size_t) window48 * sizeof (float));
    }

    // DOP dither restarts every window from the fixed table (row = frame).
    if (! MossFormerFrontend::buildFeatsAndRun (*constants, scratch, winBuf,
                                                framesPerDop, 0,
                                                MossFormerFrontend::DitherMode::dopTable,
                                                dopTable.get(), feats2, mask))
        return false;

    MossFormerFrontend::renderWet (*constants, scratch, winBuf, mask, wetBuf,
                                   amount01, framesPerDop, window48, trim48, emitOffset);
    const int stereoCh = juce::jmin (2, numCh);
    for (int ch = 0; ch < stereoCh; ++ch)
        out48[(size_t) ch].insert (out48[(size_t) ch].end(),
                                   wetBuf[(size_t) ch].data(),
                                   wetBuf[(size_t) ch].data() + emitLen);
    for (int ch = 2; ch < numCh; ++ch) // extra channels duplicate channel 1
        out48[(size_t) ch].insert (out48[(size_t) ch].end(),
                                   wetBuf[1].data(),
                                   wetBuf[1].data() + emitLen);
    // Drop the consumed stride so the next window starts at the next 3 s
    // position (the overlap region is re-read by construction).
    for (auto& q : in48)
        if ((int) q.size() >= stride48)
            q.erase (q.begin(), q.begin() + stride48);
    ++windowsRun;
    return true;
}

bool MossFormerDenoise::allChannelsReady() const
{
    for (const auto& q : in48)
        if ((int) q.size() < window48)
            return false;
    return ! in48.empty();
}

void MossFormerDenoise::runFallback (juce::AudioBuffer<float>& buffer)
{
    if (dfn3 == nullptr)
        return; // nothing to fall back to: passthrough
    const int n = buffer.getNumSamples();
    for (int ch = 0; ch < numCh; ++ch)
    {
        const int src = juce::jmin (ch, buffer.getNumChannels() - 1);
        std::memcpy (inputCopy.data() + (size_t) ch * (size_t) n,
                     buffer.getReadPointer (src), (size_t) n * sizeof (float));
    }
    float* viewPtrs[2] = { inputCopy.data(),
                           numCh > 1 ? inputCopy.data() + n : inputCopy.data() };
    juce::AudioBuffer<float> view (viewPtrs, numCh, n);
    dfn3->process (view);
    for (int i = 0; i < n; ++i)
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float delayed = dfnDelay[(size_t) ch * (size_t) dfnDelayCap + (size_t) dfnDelayPos];
            dfnDelay[(size_t) ch * (size_t) dfnDelayCap + (size_t) dfnDelayPos]
                = inputCopy[(size_t) ch * (size_t) n + (size_t) i];
            const int dst = juce::jmin (ch, buffer.getNumChannels() - 1);
            buffer.getWritePointer (dst)[i] = delayed;
        }
        dfnDelayPos = (dfnDelayPos + 1) % dfnDelayCap;
    }
}

void MossFormerDenoise::process (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() <= 0)
        return;

    if (failedMidRender)
        return; // dry remainder, file is marked invalid (plan rule)

    if (fallbackChain)
    {
        runFallback (buffer);
        return;
    }

    if (constants == nullptr || dopTable == nullptr)
    {
        // resources missing: pad the DFN3 chain to the same contract
        fallbackChain = true;
        runFallback (buffer);
        return;
    }

    if (syncWait && ! waitedForLoader)
    {
        waitedForLoader = true;
        if (! MossFormerMaskNet::instance().waitReady (30000))
        {
            fallbackChain = true;
            runFallback (buffer);
            return;
        }
    }
    active = true;

    const int numChBuf = buffer.getNumChannels();

    // Pass 1: every channel feeds its resampler and window queue BEFORE any
    // inference — runWindow advances a shared window counter across channels,
    // so checking per channel while a later channel is still empty would spin
    // on re-running earlier windows forever.
    std::vector<float> block ((size_t) juce::jmax (n, 16384) + 8);
    for (int ci = 0; ci < numCh; ++ci)
    {
        if (ci < numChBuf)
            resampler.upsample (buffer.getWritePointer (ci), n, ci);
        while (true)
        {
            const int got = resampler.take48 (block.data(), (int) block.size(), ci);
            if (got <= 0)
                break;
            in48[(size_t) ci].insert (in48[(size_t) ci].end(), block.begin(),
                                      block.begin() + got);
        }
    }
    // Pass 2: run every window whose full 4 s span has arrived on EVERY
    // channel (the queue always holds the next window at its front and is
    // trimmed by one stride after each run). A failed window invalidates the
    // render instead of spinning the loop forever.
    while (allChannelsReady())
    {
        if (! runWindow (windowsRun))
        {
            // mid-render ORT failure: dry remainder from here on
            failedMidRender = true;
            active = false;
            return;
        }
    }

    for (int ci = 0; ci < numCh; ++ci)
    {
        // Hand the whole pending 48k stream to the session-domain queue.
        auto& stream = out48[(size_t) ci];
        if (resample)
        {
            if (! stream.empty())
            {
                resampler.push48 (stream.data(), (int) stream.size(), ci);
                stream.clear();
                std::vector<float> conv ((size_t) juce::jmax (n, 16384) + 8, 0.0f);
                // Ask for a lot; downsample() caps at what the queue can produce.
                const int want = (int) conv.size();
                const int written = resampler.downsample (conv.data(), want, ci);
                sessionQueue[(size_t) ci].insert (sessionQueue[(size_t) ci].end(),
                                                  conv.begin(), conv.begin() + written);
            }
        }
        else
        {
            sessionQueue[(size_t) ci].insert (sessionQueue[(size_t) ci].end(),
                                              stream.begin(), stream.end());
            stream.clear();
        }
    }

    for (int ch = 0; ch < numChBuf; ++ch)
    {
        const int ci = juce::jmin (ch, numCh - 1);
        float* x = buffer.getWritePointer (ch);
        auto& queue = sessionQueue[(size_t) ci];
        int& served = servedSession[(size_t) ci];
        int i = 0;
        // Explicit priming: the first latencySamples outputs are silence, then
        // the chunk stream starts exactly at its content-aligned position.
        if (served < latencySamples)
        {
            const int z = juce::jmin (n, latencySamples - served);
            std::memset (x, 0, (size_t) z * sizeof (float));
            served += z;
            i = z;
        }
        const int avail = (int) queue.size();
        const int use = juce::jmin (n - i, avail);
        if (use > 0)
        {
            std::memcpy (x + i, queue.data(), (size_t) use * sizeof (float));
            queue.erase (queue.begin(), queue.begin() + use);
            i += use;
        }
        if (i < n)
            std::memset (x + i, 0, (size_t) (n - i) * sizeof (float));
    }
}

#else // !STP_ENABLE_MOSSFORMER — engine unavailable: passthrough stubs

MossFormerDenoise::MossFormerDenoise() = default;
MossFormerDenoise::~MossFormerDenoise() = default;
void MossFormerDenoise::prepare (double, int, int) {}
void MossFormerDenoise::reset() {}
void MossFormerDenoise::setAmount (float) {}
void MossFormerDenoise::process (juce::AudioBuffer<float>&) {}
bool MossFormerDenoise::runWindow (int) { return false; }
bool MossFormerDenoise::allChannelsReady() const { return false; }
HqRuntimeState MossFormerDenoise::runtimeState() const { return HqRuntimeState::fallbackModelError; }

#endif
