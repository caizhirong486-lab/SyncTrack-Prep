// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

/**
 * Denoise engine modes behind the denoiseMode choice parameter.
 * Off / Classic / Live(DFN3) / HQ(MossFormer2).
 */
enum class DenoiseMode : int
{
    off = 0,
    classic = 1,
    live = 2,
    hq = 3,
};

inline constexpr int numDenoiseModes = 4;

inline const char* denoiseModeName (int mode)
{
    switch (static_cast<DenoiseMode> (mode))
    {
        case DenoiseMode::off:     return "Off";
        case DenoiseMode::classic: return "Classic";
        case DenoiseMode::live:    return "Live (DFN3)";
        case DenoiseMode::hq:      return "HQ (MossFormer2)";
    }
    return "Off";
}

/** Sample rate the NN engines natively run at. */
inline constexpr double nnEngineSampleRate = 48000.0;

/**
 * Shared sample-rate adapter for the 48 kHz-only NN engines.
 *
 * Upsamples the session stream into a 48 kHz queue and downsamples the 48 kHz
 * output queue back to the session rate. The interpolators keep their own
 * history across calls, so leftover queue samples are simply carried over.
 * Round-trip latency is reported in session-domain samples; a sub-sample
 * remainder is folded into the engines' latency rounding.
 */
class StageResampler
{
public:
    void prepare (double sessionRate, int numChannels, int maxBlock)
    {
        active = std::abs (sessionRate - nnEngineSampleRate) > 1.0;
        upRatio = sessionRate / nnEngineSampleRate;     // session in per 48k out
        downRatio = nnEngineSampleRate / sessionRate;   // 48k in per session out
        const double base = juce::Interpolators::Lagrange::getBaseLatency();
        // Base latency is stated in each interpolator's OUTPUT domain; convert
        // both contributions into session-domain samples.
        latency = base * upRatio + base;
        chans = juce::jmax (1, numChannels);
        up.resize ((size_t) chans);
        down.resize ((size_t) chans);

        // Everything the audio thread touches is sized here: the queues are
        // fixed-capacity ring-free vectors with a generous headroom, so
        // process() only ever memmoves inside them.
        const int block48 = (int) std::ceil ((double) juce::jmax (1, maxBlock) / juce::jmax (0.01, upRatio)) + 8;
        cap48 = juce::jmax (4096, block48 * 8);
        capSession = juce::jmax (4096, juce::jmax (1, maxBlock) * 8);
        sessionIn.assign ((size_t) chans, std::vector<float> ((size_t) capSession, 0.0f));
        sessionInLen.assign ((size_t) chans, 0);
        queue48.assign ((size_t) chans, std::vector<float> ((size_t) cap48, 0.0f));
        queue48Len.assign ((size_t) chans, 0);
        out48.assign ((size_t) chans, std::vector<float> ((size_t) cap48, 0.0f));
        out48Len.assign ((size_t) chans, 0);
        scratchIn.assign ((size_t) capSession, 0.0f);
        scratchOut.assign ((size_t) cap48, 0.0f);
        reset();
    }

    bool isActive() const { return active; }

    double latencySamples() const { return active ? latency : 0.0; }

    void reset()
    {
        for (auto& i : up) i.reset();
        for (auto& i : down) i.reset();
        std::fill (sessionInLen.begin(), sessionInLen.end(), 0);
        std::fill (queue48Len.begin(), queue48Len.end(), 0);
        std::fill (out48Len.begin(), out48Len.end(), 0);
    }

    /** Push session-rate input; converts as much as possible into the 48 kHz
        queue that take48() reads. */
    void upsample (const float* src, int numSessionSamples, int channel)
    {
        auto& q48 = queue48[(size_t) channel];
        int& n48 = queue48Len[(size_t) channel];

        if (! active)
        {
            const int room = cap48 - n48;
            const int n = juce::jmin (numSessionSamples, room);
            std::memcpy (q48.data() + n48, src, (size_t) n * sizeof (float));
            n48 += n;
            return;
        }

        auto& sIn = sessionIn[(size_t) channel];
        int& sLen = sessionInLen[(size_t) channel];
        const int room = capSession - sLen;
        const int take = juce::jmin (numSessionSamples, room);
        std::memcpy (sIn.data() + sLen, src, (size_t) take * sizeof (float));
        sLen += take;

        const int want = juce::jmin ((int) std::floor ((double) sLen / upRatio),
                                     cap48 - n48);
        if (want <= 0)
            return;
        std::memcpy (scratchIn.data(), sIn.data(), (size_t) sLen * sizeof (float));
        const int used = up[(size_t) channel].process (upRatio, scratchIn.data(),
                                                       q48.data() + n48, want,
                                                       sLen, 0);
        n48 += want;
        const int keep = juce::jmax (0, sLen - used);
        if (keep > 0 && used > 0)
            std::memmove (sIn.data(), sIn.data() + used, (size_t) keep * sizeof (float));
        sLen = keep;
    }

    /** Take up to maxSamples of 48 kHz input for the engine (consumes it). */
    int take48 (float* dst48, int maxSamples, int channel)
    {
        auto& q = queue48[(size_t) channel];
        int& len = queue48Len[(size_t) channel];
        const int n = juce::jmin (maxSamples, len);
        if (n <= 0)
            return 0;
        std::memcpy (dst48, q.data(), (size_t) n * sizeof (float));
        const int keep = len - n;
        if (keep > 0)
            std::memmove (q.data(), q.data() + n, (size_t) keep * sizeof (float));
        len = keep;
        return n;
    }

    /** Push 48 kHz engine output into the downsample queue. */
    void push48 (const float* src48, int num48Samples, int channel)
    {
        auto& q = out48[(size_t) channel];
        int& len = out48Len[(size_t) channel];
        const int n = juce::jmin (num48Samples, cap48 - len);
        if (n <= 0)
            return;
        std::memcpy (q.data() + len, src48, (size_t) n * sizeof (float));
        len += n;
    }

    /** Pull up to numSessionSamples of session-rate output; never invents
        samples (returns the number written, callers pad if they must). */
    int downsample (float* dst, int numSessionSamples, int channel)
    {
        auto& q = out48[(size_t) channel];
        int& len = out48Len[(size_t) channel];
        if (len <= 0)
            return 0;

        if (! active)
        {
            const int n = juce::jmin (numSessionSamples, len);
            std::memcpy (dst, q.data(), (size_t) n * sizeof (float));
            const int keep = len - n;
            if (keep > 0)
                std::memmove (q.data(), q.data() + n, (size_t) keep * sizeof (float));
            len = keep;
            return n;
        }

        // Never ask the interpolator for more than the queue can produce: its
        // shortfall path would zero-fill mid-stream.
        const int producible = (int) std::floor ((double) len / downRatio);
        const int want = juce::jmin (numSessionSamples, producible);
        if (want <= 0)
            return 0;
        std::memcpy (scratchOut.data(), q.data(), (size_t) len * sizeof (float));
        const int used = down[(size_t) channel].process (downRatio, scratchOut.data(),
                                                         dst, want, len, 0);
        const int keep = juce::jmax (0, len - used);
        if (keep > 0 && used > 0)
            std::memmove (q.data(), q.data() + used, (size_t) keep * sizeof (float));
        len = keep;
        return want;
    }

private:
    bool active = false;
    double upRatio = 1.0, downRatio = 1.0, latency = 0.0;
    int chans = 2;
    int cap48 = 4096, capSession = 4096;

    std::vector<juce::Interpolators::Lagrange> up, down;
    std::vector<std::vector<float>> sessionIn;  // pending session-domain input
    std::vector<int> sessionInLen;
    std::vector<std::vector<float>> queue48;    // 48k input awaiting the engine
    std::vector<int> queue48Len;
    std::vector<std::vector<float>> out48;      // 48k engine output
    std::vector<int> out48Len;
    std::vector<float> scratchIn, scratchOut;
};

/**
 * One denoise engine. The processor owns one instance per mode (pre-built in
 * prepareToPlay) and hot-swaps pointers with a short crossfade.
 */
class DenoiseStage
{
public:
    virtual ~DenoiseStage() = default;

    virtual void prepare (double sampleRate, int maxBlock, int numChannels) = 0;
    virtual void reset() = 0;
    /** Unified strength 0..1. Classic: over-subtraction amount; DFN3:
        attenuation limit (0% = 0 dB = bypass); HQ: wet/dry balance. */
    virtual void setAmount (float amount01) = 0;
    virtual void process (juce::AudioBuffer<float>& buffer) = 0;
    /** Total delay in session-domain samples; call after prepare(). */
    virtual int getLatencySamples() const = 0;
    /** False = must never run against a real-time deadline (HQ). */
    virtual bool supportsRealtime() const = 0;
};
