// SPDX-License-Identifier: AGPL-3.0-or-later
// 4s DOP engine: gold equivalence against the retired fixed ONNX graph
// (the historical benchmark), plus determinism and first-window rules.
#include <catch2/catch_test_macros.hpp>

#include "dsp/MossFormerDenoise.h"
#include "TestRequireNN.h"

#include <cmath>
#include <cstring>
#include <random>

#ifdef STP_ENABLE_MOSSFORMER
#include <onnxruntime_cxx_api.h>
#endif

namespace
{
juce::File modelFile()
{
    if (auto* env = std::getenv ("STP_MOSS_DYNAMIC"))
        return juce::File (juce::String (env));
    return juce::File::getCurrentWorkingDirectory()
               .getChildFile ("third_party/mossformer2/mossformer2_dynamic.onnx");
}

juce::File legacyGoldFile()
{
    if (auto* env = std::getenv ("STP_MOSS_MODEL"))
        return juce::File (juce::String (env));
    return juce::File::getCurrentWorkingDirectory()
               .getChildFile ("third_party/mossformer2/mossformer2_fp32.onnx");
}

juce::AudioBuffer<float> makeSignal (int n)
{
    juce::AudioBuffer<float> in (2, n);
    std::mt19937 rng (7);
    std::normal_distribution<float> dist (0.0f, 0.02f);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / 48000.0f;
        const float s = 0.1f * std::sin (2.0f * juce::MathConstants<float>::pi
                                         * (200.0f + 30.0f * t) * t)
                        + dist (rng);
        in.setSample (0, i, s);
        in.setSample (1, i, s * 0.85f);
    }
    return in;
}
} // namespace

TEST_CASE ("4s DOP window matches the retired fixed-graph gold", "[moss][dop]")
{
#ifdef STP_ENABLE_MOSSFORMER
    stpRequireNnOrSkip (modelFile().existsAsFile() && legacyGoldFile().existsAsFile(),
                        "dynamic / gold model not present");

    const int n = MossFormerDenoise::window48; // exactly one window
    const auto in = makeSignal (n);

    // Reference: the retired fixed waveform->waveform graph, first-window
    // rule (emit [0, 3.5 s)), one mono pass per channel.
    Ort::Env env (ORT_LOGGING_LEVEL_WARNING, "SyncTrackPrepTests-Gold");
    Ort::Session gold (env, legacyGoldFile().getFullPathName().toStdString().c_str(),
                       Ort::SessionOptions());
    juce::AudioBuffer<float> ref (2, (int) MossFormerDenoise::window48 - MossFormerDenoise::trim48);
    for (int ch = 0; ch < 2; ++ch)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = in.getSample (ch, i);
        std::array<int64_t, 2> dims { 1, (int64_t) n };
        auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float> (mem, x.data(), x.size(),
                                                       dims.data(), dims.size());
        const char* inputNames[] = { "input" };
        const char* outputNames[] = { "output" };
        auto out = gold.Run (Ort::RunOptions { nullptr }, inputNames, &tensor, 1,
                             outputNames, 1);
        REQUIRE (! out.empty());
        std::memcpy (ref.getWritePointer (ch), out[0].GetTensorData<float>(),
                     ref.getNumSamples() * sizeof (float));
    }

    // Candidate: the dynamic MaskNet engine through the C++ frontend/backend.
    MossFormerDenoise e;
    e.setModelPath (modelFile());
    e.setMelPath (juce::File::getCurrentWorkingDirectory()
                      .getChildFile ("third_party/mossformer2/mel60_2048.f32"));
    e.setDopDitherPath (juce::File::getCurrentWorkingDirectory()
                            .getChildFile ("third_party/mossformer2/dop_dither.f32"));
    e.setSyncWait (true);
    e.setAmount (1.0f);
    e.prepare (48000.0, 512, 2);
    stpRequireNnOrSkip (MossFormerMaskNet::instance().waitReady (30000),
                        "MaskNet session failed to load");
    REQUIRE (e.isLoaded());

    // Render 4 s of input (window 0 completes on the last block), then flush
    // 4 s of silence so the chunk stream drains, collecting the output stream.
    const int flush = MossFormerDenoise::window48;
    juce::AudioBuffer<float> rendered (2, n + flush);
    juce::AudioBuffer<float> slice (2, 512);
    for (int off = 0; off < rendered.getNumSamples(); off += 512)
    {
        const int len = juce::jmin (512, rendered.getNumSamples() - off);
        for (int ch = 0; ch < 2; ++ch)
        {
            if (off < n)
                slice.copyFrom (ch, 0, in, ch, off, len);
            else
                slice.clear();
        }
        e.process (slice);
        for (int ch = 0; ch < 2; ++ch)
            rendered.copyFrom (ch, off, slice, ch, 0, len);
    }

    // The engine output is content-aligned at latencySamples: output position
    // latency + i corresponds to input sample i. Window 0 emits [0, 3.5 s).
    const int lat = e.getLatencySamples();
    const int emit = ref.getNumSamples();
    double dot = 0.0, na = 0.0, nb = 0.0;
    double maxAbsDelta = 0.0;
    double refRms = 0.0, gotRms = 0.0;
    for (int i = 0; i < emit; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const double a = ref.getSample (ch, i);
            const double b = rendered.getSample (ch, lat + i);
            dot += a * b;
            na += a * a;
            nb += b * b;
            refRms += a * a;
            gotRms += b * b;
            maxAbsDelta = juce::jmax (maxAbsDelta, std::abs (a - b));
        }
    }
    const double corr = dot / (std::sqrt (na) * std::sqrt (nb) + 1e-20);
    const double rmsDeltaDb = 20.0 * std::log10 ((std::sqrt (gotRms / (2.0 * emit)) + 1e-20)
                                                 / (std::sqrt (refRms / (2.0 * emit)) + 1e-20));
    INFO ("DOP gold corr " << corr << " rms delta " << rmsDeltaDb << " dB");
    INFO ("windowsRun " << e.debugWindowsRun() << " lat " << lat);
    REQUIRE (corr >= 0.9999);
    REQUIRE (std::abs (rmsDeltaDb) < 0.1);

    SECTION ("deterministic across renders")
    {
        MossFormerDenoise e2;
        e2.setModelPath (modelFile());
        e2.setMelPath (juce::File::getCurrentWorkingDirectory()
                           .getChildFile ("third_party/mossformer2/mel60_2048.f32"));
        e2.setDopDitherPath (juce::File::getCurrentWorkingDirectory()
                                 .getChildFile ("third_party/mossformer2/dop_dither.f32"));
        e2.setSyncWait (true);
        e2.setAmount (1.0f);
        e2.prepare (48000.0, 512, 2);
        juce::AudioBuffer<float> rendered2 (2, n + flush);
        for (int off = 0; off < rendered2.getNumSamples(); off += 512)
        {
            const int len = juce::jmin (512, rendered2.getNumSamples() - off);
            for (int ch = 0; ch < 2; ++ch)
            {
                if (off < n)
                    slice.copyFrom (ch, 0, in, ch, off, len);
                else
                    slice.clear();
            }
            e2.process (slice);
            for (int ch = 0; ch < 2; ++ch)
                rendered2.copyFrom (ch, off, slice, ch, 0, len);
        }
        double maxDiff = 0.0;
        for (int i = 0; i < rendered.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                maxDiff = juce::jmax (maxDiff,
                                      (double) std::abs (rendered.getSample (ch, i)
                                                         - rendered2.getSample (ch, i)));
        INFO ("render max diff " << maxDiff);
        REQUIRE (maxDiff <= 1.0e-6);
    }
#else
    SKIP ("built without STP_ENABLE_MOSSFORMER");
#endif
}
