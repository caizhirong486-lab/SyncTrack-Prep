// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Dfn3Denoise.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#ifdef STP_ENABLE_DFN3

void Dfn3Denoise::prepare (double sampleRate, int maxBlock, int numChannels)
{
    sessionRate = sampleRate;
    resampler.prepare (sampleRate, juce::jmax (1, numChannels), juce::jmax (1, maxBlock));
    resample = resampler.isActive();
    engineRate = resample ? nnEngineSampleRate : sessionRate;
    maxBlock48 = juce::jmax (512, (int) std::ceil ((double) maxBlock * engineRate / sessionRate) + frameLen);

    // (Re)create states on every prepare: sample-rate or block changes and
    // resets both go through here, and the C API has no other state reset.
    for (auto* s : states)
        if (s != nullptr)
            df_free (s);
    states.clear();
    if (modelFile.existsAsFile() && numChannels > 0)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            // atten_lim: 0 = bypass, >= 100 = unlimited (see class doc)
            states.push_back (df_create (modelFile.getFullPathName().toRawUTF8(),
                                         attenLimDb(), nullptr));
        frameLen = states[0] != nullptr ? (int) df_get_frame_length (states[0]) : 480;
    }

    const int nch = juce::jmax (1, numChannels);
    const int cap = frameLen * 2 + maxBlock48 * 2;
    inFifo.assign ((size_t) nch, std::vector<float> ((size_t) cap, 0.0f));
    outFifo.assign ((size_t) nch, std::vector<float> ((size_t) cap, 0.0f));
    fifoWrite.assign ((size_t) nch, 0);
    hopCounter.assign ((size_t) nch, 0);
    outRead.assign ((size_t) nch, 0);
    outAvail.assign ((size_t) nch, 0);
    frameIn.assign ((size_t) frameLen, 0.0f);
    frameOut.assign ((size_t) frameLen, 0.0f);
    // RT-safe scratch for the resampling path, plus the bypass ring.
    resampleIn.assign ((size_t) juce::jmax (maxBlock48, frameLen * 2), 0.0f);
    frameCarry.assign ((size_t) nch, std::vector<float> ((size_t) frameLen, 0.0f));
    frameCarried.assign ((size_t) nch, 0);
    resampleOut.assign ((size_t) juce::jmax (maxBlock48, juce::jmax (1, maxBlock) * 2), 0.0f);
    const int bypassCap = frameLen * 8 + maxBlock48;
    bypassQueue.assign ((size_t) nch, std::vector<float> ((size_t) bypassCap, 0.0f));
    bypassQueued.assign ((size_t) nch, 0);

    if (isLoaded())
    {
        const int latency48 = probeLatency48();
        latency48Cache = latency48;
        // A frame completed at sample i has its first output served in the
        // same iteration, so the true content delay is one sample below the
        // frame-aligned xcorr lag.
        const double sessionLatency = (double) (latency48 - 1) * (sessionRate / engineRate)
                                      + resampler.latencySamples();
        latencySamples = (int) std::ceil (sessionLatency);
    }
    else
    {
        latencySamples = 0;
    }
    reset();
}

void Dfn3Denoise::reset()
{
    // FIFOs only: DFState carries the model's STFT/norm state and is
    // recreated in prepare() (the C API offers no in-place reset).
    for (auto& c : frameCarry)
        std::fill (c.begin(), c.end(), 0.0f);
    std::fill (frameCarried.begin(), frameCarried.end(), 0);
    for (auto& q : bypassQueue)
        std::fill (q.begin(), q.end(), 0.0f);
    std::fill (bypassQueued.begin(), bypassQueued.end(), 0);
    for (auto& f : inFifo)
        std::fill (f.begin(), f.end(), 0.0f);
    for (auto& f : outFifo)
        std::fill (f.begin(), f.end(), 0.0f);
    std::fill (fifoWrite.begin(), fifoWrite.end(), 0);
    std::fill (hopCounter.begin(), hopCounter.end(), 0);
    std::fill (outRead.begin(), outRead.end(), 0);
    std::fill (outAvail.begin(), outAvail.end(), 0);
    resampler.reset();
}

void Dfn3Denoise::setAmount (float amount01In)
{
    amount01 = juce::jlimit (0.0f, 1.0f, amount01In);
    for (auto* s : states)
        if (s != nullptr)
            df_set_atten_lim (s, attenLimDb());
}

/** White-noise cross-correlation over whole frames: DFN is hop-aligned, so
    the content delay is an integer number of frames. Runs on the 48k domain
    state of channel 0. */
int Dfn3Denoise::probeLatency48()
{
    if (states.empty() || states[0] == nullptr)
        return 0;
    DFState* st = states[0];
    const int nFrames = 96;
    const int total = nFrames * frameLen;
    std::vector<float> in ((size_t) total), out ((size_t) total, 0.0f);
    std::mt19937 rng (1234);
    std::normal_distribution<float> dist (0.0f, 0.2f);
    for (auto& v : in)
        v = dist (rng);
    for (int f = 0; f < nFrames; ++f)
    {
        float* ip = in.data() + f * frameLen;
        float* op = out.data() + f * frameLen;
        df_process_frame (st, ip, op);
    }
    // Recreate states so the probe noise never reaches the audio path.
    for (size_t ch = 0; ch < states.size(); ++ch)
    {
        df_free (states[ch]);
        states[ch] = df_create (modelFile.getFullPathName().toRawUTF8(),
                                attenLimDb(), nullptr);
    }

    int bestLag = 0;
    double bestCorr = -1.0e300;
    for (int lagFrames = 0; lagFrames <= 8; ++lagFrames)
    {
        double acc = 0.0;
        const int start = lagFrames * frameLen;
        for (int i = start; i < total; ++i)
            acc += (double) out[(size_t) i] * in[(size_t) (i - start)];
        if (acc > bestCorr)
        {
            bestCorr = acc;
            bestLag = lagFrames;
        }
    }
    return bestLag * frameLen;
}

void Dfn3Denoise::process (juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (n <= 0 || numCh <= 0)
        return;

    if (! isLoaded())
        return; // engine absent: passthrough (prepare reports latency 0)

    const int cap = (int) outFifo[0].size();
    const int nch = juce::jmin (numCh, (int) states.size());

    if (! resample)
    {
        for (int ch = 0; ch < numCh; ++ch)
        {
            float* x = buffer.getWritePointer (ch);
            const int ci = juce::jmin (ch, nch - 1);
            for (int i = 0; i < n; ++i)
            {
                inFifo[(size_t) ci][(size_t) fifoWrite[(size_t) ci]] = x[i];
                fifoWrite[(size_t) ci] = (fifoWrite[(size_t) ci] + 1) % frameLen;
                hopCounter[(size_t) ci]++;

                if (hopCounter[(size_t) ci] >= frameLen)
                {
                    hopCounter[(size_t) ci] = 0;
                    std::memcpy (frameIn.data(), inFifo[(size_t) ci].data(),
                                 (size_t) frameLen * sizeof (float));
                    // libDF's atten_lim floors the ERB gains but the deep
                    // filter coefficients still filter; a true bypass at
                    // amount 0 must skip the model entirely — and hold back
                    // the same number of frames as the model's lookahead so
                    // the reported latency keeps matching the stream.
                    if (amount01 <= 0.0001f && latencySamples > 0)
                    {
                        auto& q = bypassQueue[(size_t) ci];
                        int& queued = bypassQueued[(size_t) ci];
                        if (queued + frameLen <= (int) q.size())
                        {
                            std::memcpy (q.data() + queued, frameIn.data(),
                                         (size_t) frameLen * sizeof (float));
                            queued += frameLen;
                        }
                        if (queued >= latency48Cache && queued >= frameLen)
                        {
                            std::memcpy (frameOut.data(), q.data(), (size_t) frameLen * sizeof (float));
                            queued -= frameLen;
                            if (queued > 0)
                                std::memmove (q.data(), q.data() + frameLen,
                                              (size_t) queued * sizeof (float));
                        }
                        else
                        {
                            std::fill (frameOut.begin(), frameOut.end(), 0.0f);
                        }
                    }
                    else
                    {
                        df_process_frame (states[(size_t) ci], frameIn.data(), frameOut.data());
                    }
                    for (int k = 0; k < frameLen; ++k)
                    {
                        const int w = (outRead[(size_t) ci] + outAvail[(size_t) ci]) % cap;
                        outFifo[(size_t) ci][(size_t) w] = frameOut[(size_t) k];
                        ++outAvail[(size_t) ci];
                        if (outAvail[(size_t) ci] > cap)
                        {
                            outRead[(size_t) ci] = (outRead[(size_t) ci] + 1) % cap;
                            outAvail[(size_t) ci] = cap;
                        }
                    }
                }

                if (outAvail[(size_t) ci] > 0)
                {
                    x[i] = outFifo[(size_t) ci][(size_t) outRead[(size_t) ci]];
                    outRead[(size_t) ci] = (outRead[(size_t) ci] + 1) % cap;
                    --outAvail[(size_t) ci];
                }
                else
                {
                    x[i] = 0.0f; // priming the frame pipeline
                }
            }
        }
        return;
    }

    // Session rate != 48k: resample in, run frames, resample out. Both
    // scratch buffers were sized in prepare() — no audio-thread allocation.
    float* s48 = resampleIn.data();
    float* sess = resampleOut.data();
    for (int ch = 0; ch < numCh; ++ch)
    {
        const int ci = juce::jmin (ch, nch - 1);
        float* x = buffer.getWritePointer (ch);
        resampler.upsample (x, n, ci);
        // Drain every whole frame the upsampler has produced. take48()
        // consumes what it returns, so a short read has to be carried over to
        // the next block — dropping it starved the model and produced near
        // silence at every non-48k session rate.
        auto& carry = frameCarry[(size_t) ci];
        int& carried = frameCarried[(size_t) ci];
        while (true)
        {
            const int got = resampler.take48 (s48, frameLen - carried, ci);
            if (got <= 0)
                break;
            std::memcpy (carry.data() + carried, s48, (size_t) got * sizeof (float));
            carried += got;
            if (carried < frameLen)
                break;
            df_process_frame (states[(size_t) ci], carry.data(), frameOut.data());
            resampler.push48 (frameOut.data(), frameLen, ci);
            carried = 0;
        }
        // Convert the pending 48k stream back to session rate; pad zeros while
        // the pipeline is still priming (covered by the reported latency).
        const int written = resampler.downsample (sess, n, ci);
        if (written < n)
            std::memset (sess + written, 0, (size_t) (n - written) * sizeof (float));
        std::memcpy (x, sess, (size_t) n * sizeof (float));
    }
}

#else // !STP_ENABLE_DFN3 — engine unavailable: passthrough stubs

void Dfn3Denoise::prepare (double, int, int) { latencySamples = 0; }
void Dfn3Denoise::reset() {}
void Dfn3Denoise::setAmount (float) {}
void Dfn3Denoise::process (juce::AudioBuffer<float>&) {}
int Dfn3Denoise::probeLatency48() { return 0; }

#endif
