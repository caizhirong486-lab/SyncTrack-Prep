// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MossFormerFrontend.h"

#include "MossFormerMaskNet.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

float hammingAt (int n, int N)
{
    // torch.hamming_window(N, periodic=False): 0.54 - 0.46*cos(2*pi*n/(N-1))
    return (float) (0.54 - 0.46 * std::cos (kTwoPi * (double) n / (double) (N - 1)));
}
} // namespace

std::shared_ptr<const MossFormerFrontend::Constants>
MossFormerFrontend::loadConstants (const juce::File& melFile, juce::String& error)
{
    constexpr size_t expectedMel = 60 * 1024 * sizeof (float);
    juce::MemoryBlock data;
    juce::FileInputStream stream (melFile);
    const bool ok = stream.openedOk()
                    && stream.readIntoMemoryBlock (data, (ssize_t) expectedMel) == (ssize_t) expectedMel
                    && data.getSize() == expectedMel;
    if (! ok)
    {
        error = "mel bank missing or corrupt: " + melFile.getFullPathName();
        return nullptr;
    }

    auto c = std::make_shared<Constants>();
    c->mel.resize (60 * 1024);
    std::memcpy (c->mel.data(), data.getData(), expectedMel);

    for (int n = 0; n < kWinLen; ++n)
        c->window[(size_t) n] = hammingAt (n, kWinLen);

    for (int k = 0; k < kBins; ++k)
        c->binw[(size_t) k] = (k == 0 || k == kBins - 1) ? 1.0f : 2.0f;

    c->stftCos.resize ((size_t) kBins * kWinLen);
    c->stftSin.resize ((size_t) kBins * kWinLen);
    c->istftCos.resize ((size_t) kWinLen * kBins);
    c->istftSin.resize ((size_t) kWinLen * kBins);
    for (int k = 0; k < kBins; ++k)
        for (int n = 0; n < kWinLen; ++n)
        {
            const double ang = kTwoPi * (double) k * (double) n / (double) kWinLen;
            c->stftCos[(size_t) k * kWinLen + (size_t) n] = (float) std::cos (ang);
            c->stftSin[(size_t) k * kWinLen + (size_t) n] = (float) std::sin (ang);
            c->istftCos[(size_t) n * kBins + (size_t) k] = (float) std::cos (ang);
            c->istftSin[(size_t) n * kBins + (size_t) k] = (float) std::sin (ang);
        }
    return c;
}

std::shared_ptr<const std::vector<float>>
MossFormerFrontend::loadDopDither (const juce::File& file, juce::String& error)
{
    juce::MemoryBlock data;
    if (! file.existsAsFile() || ! file.loadFileAsData (data)
        || data.getSize() != (size_t) (496 * 1920 * sizeof (float)))
    {
        error = "DOP dither table missing or corrupt: " + file.getFullPathName();
        return nullptr;
    }
    auto table = std::make_shared<std::vector<float>> (496 * 1920);
    std::memcpy (table->data(), data.getData(), (size_t) data.getSize());
    return table;
}

void MossFormerFrontend::shortDitherFrame (std::uint64_t globalFrameIndex, float* out)
{
    constexpr std::uint64_t kMask = 0xFFFFFFFFFFFFFFFFULL;
    // matches the Python golden: state = splitmix64(SEED ^ ((idx * K) & MASK))
    std::uint64_t state = 20260906ULL ^ ((globalFrameIndex * 0x9E3779B97F4A7C15ULL) & kMask);
    state = (state + 0x9E3779B97F4A7C15ULL) & kMask;
    state = ((state ^ (state >> 30)) * 0xBF58476D1CE4E5B9ULL) & kMask;
    state = ((state ^ (state >> 27)) * 0x94D049BB133111EBULL) & kMask;
    state ^= (state >> 31);
    auto next = [&state]
    {
        state = (state + 0x9E3779B97F4A7C15ULL) & kMask;
        state = ((state ^ (state >> 30)) * 0xBF58476D1CE4E5B9ULL) & kMask;
        state = ((state ^ (state >> 27)) * 0x94D049BB133111EBULL) & kMask;
        return state ^ (state >> 31);
    };
    for (int i = 0; i < 1920; i += 2)
    {
        state = next();
        double u1 = (double) (state >> 11) * 1.1102230246251565e-16;  // 2^-53
        if (u1 == 0.0)
            u1 = 1.1102230246251565e-16;
        state = next();
        const double u2 = (double) (state >> 11) * 1.1102230246251565e-16;
        const double r = std::sqrt (-2.0 * std::log (u1));
        out[i] = (float) (r * std::cos (kTwoPi * u2));
        if (i + 1 < 1920)
            out[i + 1] = (float) (r * std::sin (kTwoPi * u2));
    }
}

void MossFormerFrontend::computeDeltas (const Constants&, Scratch& s,
                                        const float* timeMajor, int frames,
                                        std::vector<float>& out)
{
    // torchaudio.functional.compute_deltas with its default win_length=5:
    // d_t = sum_{n=1..2} n*(c[t+n]-c[t-n]) / (2*sum n^2 = 10), replicate pad.
    out.assign ((size_t) frames * 60, 0.0f);
    auto at = [&] (int ch, int t)
    {
        t = juce::jlimit (0, frames - 1, t);
        return timeMajor[(size_t) ch * (size_t) frames + (size_t) t];
    };
    for (int ch = 0; ch < 60; ++ch)
        for (int t = 0; t < frames; ++t)
        {
            const double d1 = (double) at (ch, t + 1) - (double) at (ch, t - 1);
            const double d2 = (double) at (ch, t + 2) - (double) at (ch, t - 2);
            out[(size_t) t * 60 + (size_t) ch] = (float) ((d1 + 2.0 * d2) / 10.0);
        }
}

void MossFormerFrontend::computeFeats (const Constants& c, Scratch& s,
                                       const float* audio, int frames,
                                       float* featsOut,
                                       const DitherPlan* plan)
{
    // audio holds frames*kHop + kWinLen normalised-domain samples; the int16
    // scaling happens here (ClearerVoice reference decodes in that domain).
    s.fbank.assign ((size_t) frames * 60, 0.0f);
    s.frame.resize (kFft);
    s.power.resize (1024);
    std::vector<float> dith;
    if (plan != nullptr)
        dith.resize (kWinLen);

    for (int t = 0; t < frames; ++t)
    {
        const float* src = audio + (size_t) t * kHop;
        if (plan != nullptr)
        {
            if (plan->mode == DitherMode::shortHash)
            {
                shortDitherFrame (plan->frameBase + (std::uint64_t) t, dith.data());
            }
            else
            {
                const std::uint64_t row = plan->frameBase + (std::uint64_t) t;
                if (plan->dopTable != nullptr
                    && row < (std::uint64_t) plan->dopTable->size() / (std::uint64_t) kWinLen)
                    std::memcpy (dith.data(),
                                 plan->dopTable->data() + (size_t) row * (size_t) kWinLen,
                                 (size_t) kWinLen * sizeof (float));
                else
                    std::fill (dith.begin(), dith.end(), 0.0f);
            }
        }
        // int16 domain, dither injected before remove_dc_offset
        float frame32 [kWinLen];
        for (int i = 0; i < kWinLen; ++i)
            frame32[i] = (src[i] + (plan != nullptr ? dith[(size_t) i] : 0.0f)) * 32768.0f;

        // remove_dc_offset
        double mean = 0.0;
        for (int i = 0; i < kWinLen; ++i)
            mean += (double) frame32[i];
        mean /= (double) kWinLen;
        // preemphasis 0.97 with kaldi replicate pad (first sample: 0.03*x[0])
        for (int i = 0; i < kWinLen; ++i)
        {
            const float x = (float) ((double) frame32[i] - mean);
            const float prev = i == 0 ? x : (float) ((double) frame32[i - 1] - mean);
            s.frame[(size_t) i] = i == 0 ? 0.03f * x : x - 0.97f * prev;
        }
        for (int i = 0; i < kWinLen; ++i)
            s.frame[(size_t) i] *= c.window[(size_t) i];
        std::fill (s.frame.begin() + kWinLen, s.frame.end(), 0.0f);

        rfftPower (c, s, s.frame.data(), s.power.data());

        // mel = power[0..1024) @ mel.T -> log(clamp(eps))
        float* dst = s.fbank.data() + (size_t) t * 60;
        for (int m = 0; m < 60; ++m)
        {
            const float* row = c.mel.data() + (size_t) m * 1024;
            // float accumulation mirrors torchaudio's float32 matmul closely
            // enough to sit inside its own rounding band (double accumulation
            // actually drifts further from the reference).
            float acc = 0.0f;
            for (int b = 0; b < 1024; ++b)
                acc += s.power[(size_t) b] * row[b];
            dst[m] = std::log (std::max (acc, std::numeric_limits<float>::epsilon()));
        }
    }

    // deltas need time-major [60, frames] input
    s.d1.assign ((size_t) frames * 60, 0.0f);
    for (int f = 0; f < frames; ++f)
        for (int m = 0; m < 60; ++m)
            s.d1[(size_t) m * (size_t) frames + (size_t) f] = s.fbank[(size_t) f * 60 + (size_t) m];
    computeDeltas (c, s, s.d1.data(), frames, s.d2);   // s.d2 = delta [t*60+m]

    for (int t = 0; t < frames; ++t)
    {
        float* o = featsOut + (size_t) t * 180;
        std::memcpy (o, s.fbank.data() + (size_t) t * 60, 60 * sizeof (float));
        std::memcpy (o + 60, s.d2.data() + (size_t) t * 60, 60 * sizeof (float));
    }
    // second-order deltas: run computeDeltas on the first-order delta
    s.d1.assign ((size_t) 60 * frames, 0.0f);
    for (int f = 0; f < frames; ++f)
        for (int m = 0; m < 60; ++m)
            s.d1[(size_t) m * (size_t) frames + (size_t) f] = s.d2[(size_t) f * 60 + (size_t) m];
    std::vector<float> d2out;
    computeDeltas (c, s, s.d1.data(), frames, d2out);
    for (int t = 0; t < frames; ++t)
        std::memcpy (featsOut + (size_t) t * 180 + 120, d2out.data() + (size_t) t * 60, 60 * sizeof (float));
}

bool MossFormerFrontend::buildFeatsAndRun (const Constants& c, Scratch& s,
                                           std::array<std::vector<float>, 2>& win,
                                           int frames, std::uint64_t frameBase,
                                           DitherMode mode,
                                           const std::vector<float>* dopTable,
                                           std::vector<float>& feats2,
                                           std::vector<float>& mask)
{
    const int winSamples = frames * kHop + kWinLen;
    feats2.assign ((size_t) 2 * frames * kFeat, 0.0f);
    DitherPlan plan;
    plan.mode = mode;
    plan.frameBase = mode == DitherMode::dopTable ? 0 : frameBase;
    plan.dopTable = dopTable;
    for (int ch = 0; ch < 2; ++ch)
    {
        if ((int) s.winScratch.size() < winSamples)
            s.winScratch.resize ((size_t) winSamples);
        std::memcpy (s.winScratch.data(), win[(size_t) ch].data(),
                     (size_t) winSamples * sizeof (float));
        computeFeats (c, s, s.winScratch.data(), frames,
                      feats2.data() + (size_t) ch * frames * kFeat, &plan);
    }
    return MossFormerMaskNet::instance().run (feats2.data(), 2, frames, mask);
}

void MossFormerFrontend::renderWet (const Constants& c, Scratch& s,
                                    std::array<std::vector<float>, 2>& win,
                                    std::vector<float>& mask,
                                    std::array<std::vector<float>, 2>& wet,
                                    float amountWet, int frames, int winSamples,
                                    int trim, int emitOffset)
{
    const int emitLen = winSamples - trim - emitOffset;
    for (int ch = 0; ch < 2; ++ch)
    {
        computeSpec (c, s, win[(size_t) ch].data(), winSamples, s.specRe, s.specIm);
        istftMasked (c, s, s.specRe, s.specIm,
                     mask.data() + (size_t) ch * frames * kBins, frames,
                     wet[(size_t) ch]);
        auto& out = wet[(size_t) ch];
        for (int i = 0; i < winSamples; ++i)
            out[(size_t) i] *= (1.0f / 32768.0f);
        if (amountWet < 0.9999f)
        {
            const auto& dry = win[(size_t) ch];
            for (int i = 0; i < winSamples; ++i)
                out[(size_t) i] = amountWet * out[(size_t) i]
                                  + (1.0f - amountWet) * dry[(size_t) i];
        }
        std::memmove (out.data(), out.data() + emitOffset, (size_t) emitLen * sizeof (float));
    }
}

void MossFormerFrontend::rfftPower (const Constants&, Scratch& s,
                                    const float* frame, float* power1024)
{
    static thread_local juce::dsp::FFT fft (11);  // 2048
    std::array<float, 4096> buf {};
    std::memcpy (buf.data(), frame, kFft * sizeof (float));
    fft.performRealOnlyForwardTransform (buf.data(), true);
    for (int b = 0; b < 1024; ++b)
    {
        const double re = buf[(size_t) 2 * b];
        const double im = buf[(size_t) 2 * b + 1];
        power1024[b] = (float) (re * re + im * im);
    }
}

void MossFormerFrontend::computeSpec (const Constants& c, Scratch& s,
                                      const float* audio, int numSamples,
                                      std::vector<float>& re, std::vector<float>& im)
{
    const int frames = c.framesFor (numSamples);
    re.assign ((size_t) frames * kBins, 0.0f);
    im.assign ((size_t) frames * kBins, 0.0f);
    s.windowed.resize (kWinLen);

    for (int t = 0; t < frames; ++t)
    {
        const float* src = audio + (size_t) t * kHop;
        for (int i = 0; i < kWinLen; ++i)
            s.windowed[(size_t) i] = src[i] * 32768.0f * c.window[(size_t) i];
        float* dstRe = re.data() + (size_t) t * kBins;
        float* dstIm = im.data() + (size_t) t * kBins;
        for (int k = 0; k < kBins; ++k)
        {
            const float* cosRow = c.stftCos.data() + (size_t) k * kWinLen;
            const float* sinRow = c.stftSin.data() + (size_t) k * kWinLen;
            double accR = 0.0, accI = 0.0;
            for (int n = 0; n < kWinLen; ++n)
            {
                accR += (double) s.windowed[(size_t) n] * (double) cosRow[n];
                accI -= (double) s.windowed[(size_t) n] * (double) sinRow[n];
            }
            dstRe[k] = (float) accR;
            dstIm[k] = (float) accI;
        }
    }
}

void MossFormerFrontend::istftMasked (const Constants& c, Scratch& s,
                                      const std::vector<float>& re,
                                      const std::vector<float>& im,
                                      const float* mask, int frames,
                                      std::vector<float>& out)
{
    const int outLen = frames * kHop + kWinLen;
    s.ola.assign ((size_t) outLen, 0.0f);
    s.olaNorm.assign ((size_t) outLen, 0.0f);
    s.frameOut.resize (kWinLen);

    for (int t = 0; t < frames; ++t)
    {
        const float* xr = re.data() + (size_t) t * kBins;
        const float* xi = im.data() + (size_t) t * kBins;
        const float* mk = mask + (size_t) t * kBins;
        for (int n = 0; n < kWinLen; ++n)
        {
            const float* cosRow = c.istftCos.data() + (size_t) n * kBins;
            const float* sinRow = c.istftSin.data() + (size_t) n * kBins;
            double acc = 0.0;
            for (int k = 0; k < kBins; ++k)
            {
                const double w = (double) mk[k] * (double) c.binw[(size_t) k];
                acc += (double) xr[k] * w * (double) cosRow[k]
                     - (double) xi[k] * w * (double) sinRow[k];
            }
            s.frameOut[(size_t) n] = (float) (acc / (double) kWinLen) * c.window[(size_t) n];
        }
        float* dst = s.ola.data() + (size_t) t * kHop;
        for (int n = 0; n < kWinLen; ++n)
            dst[n] += s.frameOut[(size_t) n];
        float* nrm = s.olaNorm.data() + (size_t) t * kHop;
        for (int n = 0; n < kWinLen; ++n)
            nrm[n] += c.window[(size_t) n] * c.window[(size_t) n];
    }

    out.resize ((size_t) outLen);
    for (int i = 0; i < outLen; ++i)
    {
        const float norm = s.olaNorm[(size_t) i] < 1e-8f ? 1e-8f : s.olaNorm[(size_t) i];
        out[(size_t) i] = s.ola[(size_t) i] / norm;
    }
}
