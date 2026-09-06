// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "dsp/DenoiseStage.h"
#include "dsp/Dfn3Denoise.h"
#include <cmath>
#include <cstdlib>

TEST_CASE ("StageResampler round-trips a 44.1 kHz stream", "[resampler]")
{
    // The 48 kHz-only NN engines are unusable at other session rates unless
    // upsample() actually feeds take48(); a resampler that dropped its output
    // silently produced digital silence for every non-48k session.
    StageResampler r;
    const double sr = 44100.0;
    const int block = 512;
    r.prepare (sr, 2, block);
    REQUIRE (r.isActive());

    const int n = (int) sr; // 1 s
    std::vector<float> in ((size_t) n), out ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
        in[(size_t) i] = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / (float) sr);

    std::vector<float> scratch (4096);
    int written = 0;
    for (int off = 0; off + block <= n; off += block)
    {
        r.upsample (in.data() + off, block, 0);
        while (true)
        {
            const int got = r.take48 (scratch.data(), (int) scratch.size(), 0);
            if (got <= 0)
                break;
            r.push48 (scratch.data(), got, 0); // engine identity
        }
        const int w = r.downsample (out.data() + written, block, 0);
        written += w;
    }

    INFO ("written " << written << " of " << n);
    REQUIRE (written > n * 3 / 4); // the pipeline must actually produce audio

    // Skip the priming region, then the round trip must reproduce the tone.
    const int start = 2000;
    double err = 0.0, ref = 0.0;
    for (int i = start; i < written - 8; ++i)
    {
        const float a = in[(size_t) i];
        const float b = out[(size_t) i];
        err += (double) (b - a) * (b - a);
        ref += (double) a * a;
    }
    const double snrDb = 10.0 * std::log10 (ref / juce::jmax (err, 1.0e-20));
    INFO ("round-trip SNR " << snrDb << " dB");
    // Two cascaded Lagrange stages (44.1k->48k->44.1k) measure ~18 dB SNR
    // on a 220 Hz tone; the gate only has to catch a broken path.
    REQUIRE (snrDb > 15.0);
}

TEST_CASE ("DFN3 engine runs at 44.1 kHz without going silent", "[denoise][dfn3][resampler]")
{
    const char* env = std::getenv ("STP_DFN3_MODEL");
    const juce::File model = env != nullptr
        ? juce::File (juce::String (env))
        : juce::File::getCurrentWorkingDirectory()
              .getChildFile ("third_party/dfn/model/DeepFilterNet3_onnx.tar.gz");
    if (! model.existsAsFile())
    {
        SKIP ("DFN3 model not present");
    }
#ifdef STP_ENABLE_DFN3
    Dfn3Denoise den;
    den.setModelPath (model);
    den.prepare (44100.0, 512, 2);
    REQUIRE (den.isLoaded());
    den.setAmount (0.5f);

    const int n = 44100 * 2;
    juce::AudioBuffer<float> buf (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float s = 0.2f * std::sin (2.0f * juce::MathConstants<float>::pi * 300.0f * (float) i / 44100.0f);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s);
    }
    for (int off = 0; off + 512 <= n; off += 512)
    {
        juce::AudioBuffer<float> slice (2, 512);
        slice.copyFrom (0, 0, buf, 0, off, 512);
        slice.copyFrom (1, 0, buf, 1, off, 512);
        den.process (slice);
        buf.copyFrom (0, off, slice, 0, 0, 512);
        buf.copyFrom (1, off, slice, 1, 0, 512);
    }

    const int lat = den.getLatencySamples();
    const float headRms = buf.getRMSLevel (0, lat + 4410, 22050);
    const float tailRms = buf.getRMSLevel (0, n - 20000, 16000);
    INFO ("44.1k latency " << lat << ", head rms " << headRms
          << ", tail rms " << tailRms);
    // The engine attenuates a pure tone somewhat, but must not go silent.
    REQUIRE (headRms > 0.01f);
#else
    SKIP ("Built without STP_ENABLE_DFN3");
#endif
}
