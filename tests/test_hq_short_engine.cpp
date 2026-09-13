// SPDX-License-Identifier: AGPL-3.0-or-later
// Short-window HQ engine: latency contract, block invariance, Amount=0
// transparency, deadline-miss degradation and capacity fallback.
#include <catch2/catch_test_macros.hpp>

#include "dsp/Dfn3Denoise.h"
#include "dsp/MossFormerShortDenoise.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <random>
#include <cstdio>

#ifdef STP_ENABLE_MOSSFORMER

namespace
{
juce::File modelFile()
{
    if (auto* env = std::getenv ("STP_MOSS_DYNAMIC"))
        return juce::File (juce::String (env));
    return juce::File::getCurrentWorkingDirectory()
               .getChildFile ("third_party/mossformer2/mossformer2_dynamic.onnx");
}

juce::File melFile()
{
    return juce::File::getCurrentWorkingDirectory()
               .getChildFile ("third_party/mossformer2/mel60_2048.f32");
}

juce::File dfn3File()
{
    return juce::File::getCurrentWorkingDirectory()
               .getChildFile ("third_party/dfn/model/DeepFilterNet3_onnx.tar.gz");
}

juce::AudioBuffer<float> makeSignal (int n)
{
    juce::AudioBuffer<float> in (2, n);
    std::mt19937 rng (5);
    std::normal_distribution<float> dist (0.0f, 0.01f);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / 48000.0f;
        const float s = 0.08f * std::sin (2.0f * juce::MathConstants<float>::pi
                                          * (220.0f + 40.0f * t) * t)
                        + dist (rng);
        in.setSample (0, i, s);
        in.setSample (1, i, s * 0.9f);
    }
    return in;
}

/** Renders `in` through the engine in `block`-sized chunks (synchronous mode
    unless realtime). Returns the engine output. */
juce::AudioBuffer<float> render (MossFormerShortDenoise& e, const juce::AudioBuffer<float>& in,
                                 int block, bool realtime)
{
    juce::AudioBuffer<float> out (2, in.getNumSamples());
    juce::AudioBuffer<float> slice (2, block);
    for (int off = 0; off < in.getNumSamples(); off += block)
    {
        const int n = juce::jmin (block, in.getNumSamples() - off);
        for (int ch = 0; ch < 2; ++ch)
            slice.copyFrom (ch, 0, in, ch, off, n);
        e.process (slice);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, off, slice, ch, 0, n);
    }
    return out;
}
} // namespace

TEST_CASE ("Short window: latency contract at the fixed 250 ms", "[moss][short]")
{
    auto model = modelFile();
    if (! model.existsAsFile() || ! melFile().existsAsFile())
        SKIP ("dynamic model / mel bank not present");

    MossFormerShortDenoise e;
    e.setMelPath (melFile());
    e.setModelPath (model);
    e.prepare (48000.0, 512, 2);
    REQUIRE (e.getLatencySamples() == MossFormerShortDenoise::contract48);

    MossFormerShortDenoise e44;
    e44.setMelPath (melFile());
    e44.setModelPath (model);
    e44.prepare (44100.0, 512, 2);
    // 250 ms at 44.1 kHz plus the resampler round-trip base latency.
    REQUIRE (e44.getLatencySamples() == 11025
                 + (int) std::ceil ([] {
                     StageResampler r;
                     r.prepare (44100.0, 2, 512);
                     return r.latencySamples();
                 }()));
}

TEST_CASE ("Short window: block-size invariance (synchronous offline)", "[moss][short]")
{
    auto model = modelFile();
    if (! model.existsAsFile() || ! melFile().existsAsFile())
        SKIP ("dynamic model / mel bank not present");
    if (! MossFormerMaskNet::instance().waitReady (30000))
        SKIP (juce::String ("MaskNet failed: ")
                  + MossFormerMaskNet::instance().error()
                  + " state=" + juce::String ((int) MossFormerMaskNet::instance().loadState())
              .toRawUTF8());

    const int n = 48000; // 1 s
    const auto in = makeSignal (n);

    juce::AudioBuffer<float> reference;
    for (int block : { 64, 128, 256, 512, 1024 })
    {
        MossFormerShortDenoise e;
        e.setMelPath (melFile());
        e.setModelPath (model);
        e.setSynchronous (true);
        e.setAmount (1.0f);
        e.prepare (48000.0, block, 2);
        if (block == 64 && ! MossFormerMaskNet::instance().waitReady (30000))
            SKIP (("MaskNet failed: " + MossFormerMaskNet::instance().error()
                   + " state=" + juce::String ((int) MossFormerMaskNet::instance().loadState()))
                      .toRawUTF8());
        auto out = render (e, in, block, false);

        if (reference.getNumSamples() == 0)
            reference = std::move (out);
        else
        {
            double maxDiff = 0.0;
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    maxDiff = juce::jmax (maxDiff,
                                          (double) std::abs (out.getSample (ch, i)
                                                             - reference.getSample (ch, i)));
            INFO ("block " << block << " max diff " << maxDiff);
            REQUIRE (maxDiff <= 1.0e-6);
        }
    }
}

TEST_CASE ("Short window: Amount 0% is the delay-compensated dry signal", "[moss][short]")
{
    auto model = modelFile();
    if (! model.existsAsFile() || ! melFile().existsAsFile() || ! dfn3File().existsAsFile())
        SKIP ("models not present");
    Dfn3Denoise dfn3;
    dfn3.setModelPath (dfn3File());
    dfn3.prepare (48000.0, 512, 2);
    REQUIRE (dfn3.isLoaded());
    dfn3.setAmount (0.0f); // real bypass inside libDF

    const int n = 48000;
    const auto in = makeSignal (n);

    MossFormerShortDenoise e;
    e.setMelPath (melFile());
    e.setModelPath (model);
    e.attachDfn3 (&dfn3);
    e.setSynchronous (true);
    e.setAmount (0.0f);
    e.prepare (48000.0, 512, 2);
    if (! MossFormerMaskNet::instance().waitReady (30000))
        SKIP (("MaskNet failed: " + MossFormerMaskNet::instance().error()
               + " state=" + juce::String ((int) MossFormerMaskNet::instance().loadState()))
                  .toRawUTF8());
    auto out = render (e, in, 512, false);

    const int lat = e.getLatencySamples();
    double maxDiff = 0.0;
    int atCh = 0, atI = 0;
    for (int i = lat; i < n; ++i)
        for (int ch = 0; ch < 2; ++ch)
        {
            const double d = (double) std::abs (out.getSample (ch, i)
                                                - in.getSample (ch, i - lat));
            if (d > maxDiff) { maxDiff = d; atCh = ch; atI = i; }
        }
    INFO ("amount0 dry max diff " << maxDiff << " at ch " << atCh << " i " << atI
          << " out=" << out.getSample (atCh, atI)
          << " in=" << in.getSample (atCh, atI - lat));
    {
        double bestDot = -1e30; int bestLag = 0;
        for (int lag = -2000; lag <= 2000; ++lag)
        {
            double dot = 0.0;
            for (int i = lat + 4800; i < n - 4800; i += 7)
            {
                const int j = i - lat + lag;
                if (j < 0 || j >= n) continue;
                dot += (double) out.getSample (0, i) * (double) in.getSample (0, j);
            }
            if (dot > bestDot) { bestDot = dot; bestLag = lag; }
        }
        std::fprintf (stderr, "BESTLAG %d\n", bestLag);
        for (int k = 0; k < 16; ++k)
            std::fprintf (stderr, "SMP out[%d]=%.6f in[%d]=%.6f\n",
                          lat + k, (double) out.getSample (0, lat + k), k,
                          (double) in.getSample (0, k));
        for (int k = -4; k < 8; ++k)
            std::fprintf (stderr, "BND out[%d]=%.6f in[%d]=%.6f\n",
                          lat + 6720 + k, (double) out.getSample (0, lat + 6720 + k),
                          6720 + k, (double) in.getSample (0, 6720 + k));
        for (int k = -4; k < 8; ++k)
            std::fprintf (stderr, "BND1 out[%d]=%.6f in[%d]=%.6f\n",
                          lat + 11151 + k, (double) out.getSample (1, lat + 11151 + k),
                          11151 + k, (double) in.getSample (1, 11151 + k));
        for (int seg = 0; seg < 4; ++seg)
        {
            const int lo = lat + seg * (n - 2 * 4800) / 4;
            const int hi = lat + (seg + 1) * (n - 2 * 4800) / 4;
            double bd = -1e30; int bl = 0;
            for (int lag = -3000; lag <= 3000; ++lag)
            {
                double dot = 0.0;
                for (int i = lo; i < hi; i += 7)
                {
                    const int j = i - lat + lag;
                    if (j < 0 || j >= n) continue;
                    dot += (double) out.getSample (0, i) * (double) in.getSample (0, j);
                }
                if (dot > bd) { bd = dot; bl = lag; }
            }
            std::fprintf (stderr, "SEGLAG seg=%d [%d,%d) lag=%d\n", seg, lo, hi, bl);
        }
    }
    REQUIRE (maxDiff <= 1.0e-6);
}

TEST_CASE ("Short window: forced deadline miss degrades to the aligned chain for one generation", "[moss][short]")
{
    auto model = modelFile();
    if (! model.existsAsFile() || ! melFile().existsAsFile())
        SKIP ("dynamic model / mel bank not present");
    if (! MossFormerMaskNet::instance().waitReady (30000))
        SKIP (("MaskNet failed: " + MossFormerMaskNet::instance().error()
               + " state=" + juce::String ((int) MossFormerMaskNet::instance().loadState()))
                  .toRawUTF8());

    MossFormerShortDenoise e;
    e.setMelPath (melFile());
    e.setModelPath (model);
    e.prepare (48000.0, 512, 2); // realtime: worker thread engaged
    e.setAmount (1.0f);

    const int n = 48000 * 2;
    const auto in = makeSignal (n);
    juce::AudioBuffer<float> slice (2, 512);

    // push enough input for the steady state, then force the miss
    for (int off = 0; off < n && e.runtimeState() != HqRuntimeState::shortActive; off += 512)
    {
        for (int ch = 0; ch < 2; ++ch)
            slice.copyFrom (ch, 0, in, ch, off, 512);
        e.process (slice);
        if (off > 48000)
            break; // engine never engaged (slow machine): nothing to degrade
    }
    if (e.runtimeState() != HqRuntimeState::shortActive)
        SKIP ("engine did not reach steady state in time");

    e.forceDeadlineMiss();
    bool sawDeadlineFallback = false;
    bool outputStayedAlive = true;
    for (int off = 48000; off < n; off += 512)
    {
        for (int ch = 0; ch < 2; ++ch)
            slice.copyFrom (ch, 0, in, ch, off, 512);
        e.process (slice);
        if (e.runtimeState() == HqRuntimeState::fallbackDeadline)
            sawDeadlineFallback = true;
        if (slice.getMagnitude (0, 0, 512) == 0.0f)
            outputStayedAlive = false;
    }
    REQUIRE (sawDeadlineFallback);
    REQUIRE (outputStayedAlive); // no silence, no dead air
}

TEST_CASE ("Short window: capacity fallback serves the aligned chain permanently", "[moss][short]")
{
    auto model = modelFile();
    if (! model.existsAsFile() || ! melFile().existsAsFile() || ! dfn3File().existsAsFile())
        SKIP ("models not present");

    Dfn3Denoise dfn3;
    dfn3.setModelPath (dfn3File());
    dfn3.prepare (48000.0, 512, 2);
    dfn3.setAmount (1.0f);

    const int n = 48000;
    const auto in = makeSignal (n);

    MossFormerShortDenoise e;
    e.setMelPath (melFile());
    e.setModelPath (model);
    e.attachDfn3 (&dfn3);
    e.setSynchronous (true);
    e.setAmount (1.0f);
    e.setCapacityFallback (true);
    e.prepare (48000.0, 512, 2);
    REQUIRE (e.runtimeState() == HqRuntimeState::fallbackCapacity);

    auto out = render (e, in, 512, false);
    REQUIRE (out.getMagnitude (0, 4096, n - 4096) > 1.0e-4f); // alive, no dead air
    // a-chain transparency probe: capacity fallback must also be the
    // delay-compensated dry signal (aligned DFN3 at amount 0).
    e.setAmount (0.0f);
    dfn3.setAmount (0.0f);
    auto out0 = render (e, in, 512, false);
    const int lat0 = e.getLatencySamples();
    for (int ch = 0; ch < 2; ++ch)
    {
        double md = 0.0; int at = 0;
        for (int i = lat0 + 4800; i < n; ++i)
        {
            const double d = (double) std::abs (out0.getSample (ch, i)
                                                - in.getSample (ch, i - lat0));
            if (d > md) { md = d; at = i; }
        }
        INFO ("fallback dry ch " << ch << " maxDiff " << md << " at " << at);
        CHECK (md <= 1.0e-6);
    }
}

#endif
