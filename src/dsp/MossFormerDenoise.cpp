// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MossFormerDenoise.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef STP_ENABLE_MOSSFORMER
#include <onnxruntime_cxx_api.h>
#endif

#ifdef STP_ENABLE_MOSSFORMER

/** RAII ORT session + environment shared by all channels (the model is mono;
    each channel runs a separate inference over the same session). */
struct MossFormerDenoise::Session
{
    Ort::Env env;
    std::unique_ptr<Ort::Session> run;

    explicit Session (const juce::File& model)
        : env (ORT_LOGGING_LEVEL_WARNING, "SyncTrackPrep-MossFormer")
    {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads (4);
        opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        run = std::make_unique<Ort::Session> (env, model.getFullPathName().toStdString().c_str(), opts);
    }
};

MossFormerDenoise::~MossFormerDenoise() = default;
MossFormerDenoise::MossFormerDenoise() = default; // exception cleanup needs complete Session

void MossFormerDenoise::prepare (double sampleRate, int maxBlock, int numChannels)
{
    sessionRate = sampleRate;
    numCh = juce::jmax (1, numChannels);
    resampler.prepare (sampleRate, numCh, juce::jmax (1, maxBlock));
    resample = resampler.isActive();
    engineRate = resample ? nnEngineSampleRate : sessionRate;

    if (session == nullptr && modelFile.existsAsFile())
    {
        try
        {
            session = std::make_unique<Session> (modelFile);
        }
        catch (const Ort::Exception&)
        {
            session.reset();
        }
    }
    active = session != nullptr;

    const double sessionLatency = (double) latency48 * (sessionRate / engineRate)
                                  + resampler.latencySamples();
    latencySamples = (int) std::ceil (sessionLatency);
    reset();
}

void MossFormerDenoise::reset()
{
    in48.assign ((size_t) numCh, {});
    out48.assign ((size_t) numCh, {});
    sessionQueue.assign ((size_t) numCh, {});
    servedSession.assign ((size_t) numCh, 0);
    windowsRun = 0;
    resampler.reset();
}

void MossFormerDenoise::setAmount (float amount01In)
{
    amount01 = juce::jlimit (0.0f, 1.0f, amount01In);
}

bool MossFormerDenoise::runWindow (int w)
{
    // The queue is trimmed after every window, so the next window is always
    // the first 4 s of the current queue (the caller gates readiness).
    const int start = 0;
    // First window keeps output [0, window - trim) (reference first-chunk
    // rule); later windows discard trim from each edge.
    const int emitOffset = w == 0 ? 0 : trim48;
    const int emitLen = window48 - trim48 - emitOffset;
    std::vector<float> out ((size_t) emitLen, 0.0f);

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto& q = in48[(size_t) ch];
        if ((int) q.size() < start + window48)
            return false;
        std::vector<float> in (q.begin() + start, q.begin() + start + window48);
        std::vector<float> raw ((size_t) window48, 0.0f);

        std::array<int64_t, 2> dims { 1, (int64_t) window48 };
        auto memoryInfo = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        auto inputTensor = Ort::Value::CreateTensor<float> (memoryInfo, in.data(), in.size(),
                                                            dims.data(), dims.size());
        const char* inputNames[] = { "input" };
        const char* outputNames[] = { "output" };
        std::vector<Ort::Value> outputs;
        try
        {
            outputs = session->run->Run (Ort::RunOptions { nullptr },
                                         inputNames, &inputTensor, 1,
                                         outputNames, 1);
        }
        catch (const Ort::Exception&)
        {
            // Inference failure must not escape into the render thread; the
            // caller downgrades this engine to passthrough.
            return false;
        }
        if (outputs.empty() || ! outputs[0].IsTensor())
            return false;
        const auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.empty() || shape.back() < window48)
            return false; // unexpected model output shape
        const float* src = outputs[0].GetTensorData<float>();
        std::memcpy (out.data(), src + emitOffset, (size_t) emitLen * sizeof (float));
        // Amount = wet/dry balance; the dry reference is the corresponding
        // slice of the (pre-inference) input window itself.
        const float wet = amount01;
        if (wet < 0.9999f)
        {
            const auto& q = in48[(size_t) ch];
            const size_t dryStart = (size_t) emitOffset;
            for (int i = 0; i < emitLen; ++i)
                out[(size_t) i] = wet * out[(size_t) i]
                                  + (1.0f - wet) * q[dryStart + (size_t) i];
        }
        out48[(size_t) ch].insert (out48[(size_t) ch].end(), out.begin(), out.end());
    }
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

void MossFormerDenoise::process (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() <= 0)
        return;

    if (! active)
        return; // model absent: passthrough

    const int numChBuf = buffer.getNumChannels();
    std::vector<float> block ((size_t) juce::jmax (n, 16384) + 8);

    // Pass 1: every channel feeds its resampler and window queue BEFORE any
    // inference — runWindow advances a shared window counter across channels,
    // so checking per channel while a later channel is still empty would spin
    // on re-running earlier windows forever.
    for (int ci = 0; ci < numCh; ++ci)
    {
        if (ci < numChBuf)
            resampler.upsample (buffer.getWritePointer (ci), n, ci);
        while (true)
        {
            const int got = resampler.take48 (block.data(), (int) block.size(), ci);
            if (got <= 0)
                break;
            in48[(size_t) ci].insert (in48[(size_t) ci].end(), block.begin(), block.begin() + got);
        }
    }
    // Pass 2: run every window whose full 4 s span has arrived on EVERY
    // channel (the queue always holds the next window at its front and is
    // trimmed by one stride after each run). A failed window disables the
    // engine instead of spinning the loop forever.
    while (allChannelsReady())
    {
        if (! runWindow (windowsRun))
        {
            active = false;
            break;
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

struct MossFormerDenoise::Session {};
MossFormerDenoise::MossFormerDenoise() = default;
MossFormerDenoise::~MossFormerDenoise() = default;
void MossFormerDenoise::prepare (double, int, int) {}
void MossFormerDenoise::reset() {}
void MossFormerDenoise::setAmount (float) {}
void MossFormerDenoise::process (juce::AudioBuffer<float>&) {}
bool MossFormerDenoise::runWindow (int) { return false; }
bool MossFormerDenoise::allChannelsReady() const { return false; }

#endif
