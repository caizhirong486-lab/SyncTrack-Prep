// SPDX-License-Identifier: AGPL-3.0-or-later
/**
 * Offline render — NN-era chain.
 * Usage:
 *   SyncTrackPrepOffline <in.wav> <out.wav> [soft|strong|clean]
 *       [off|classic|live|hq] [amount 0-100|-1] [tone -1..1]
 *       [--tap denoise|final] [--expander 0|1] [--flush 4.0]
 *       [--dfn3 <model.tar.gz>] [--moss <model.onnx>]
 *
 * The CLI always behaves as non-realtime, so HQ (MossFormer2) runs the true
 * model here. --tap denoise stops after the denoise stage (before the tone
 * shaper) for denoiser-level metrics; the trailing --flush seconds of silence
 * drain the NN pipelines and are excluded by the analysis tooling.
 */
#include "dsp/Presets.h"
#include "dsp/ClassicDenoise.h"
#include "dsp/Dfn3Denoise.h"
#include "dsp/MossFormerDenoise.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <cstring>

namespace
{
DenoiseMode parseMode (const char* s, bool& ok)
{
    ok = true;
    if (std::strcmp (s, "off") == 0)     return DenoiseMode::off;
    if (std::strcmp (s, "classic") == 0) return DenoiseMode::classic;
    if (std::strcmp (s, "live") == 0)    return DenoiseMode::live;
    if (std::strcmp (s, "hq") == 0)      return DenoiseMode::hq;
    ok = false;
    return DenoiseMode::classic;
}
}

static void applyChain (juce::AudioBuffer<float>& buf, double sr, int preset,
                        DenoiseMode mode, float denoiseAmountPct, float tone,
                        const juce::String& tap, bool expanderOn,
                        const juce::File& dfn3Model, const juce::File& mossModel)
{
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, buf.getNumSamples()),
                                  (juce::uint32) juce::jmax (1, buf.getNumChannels()) };

    ChannelRepair cr; Leveler lv; PeakCompressor pc;
    ToneShaper ts; TruePeakLimiter tp; UpwardExpander ue;
    ClassicDenoise classic; Dfn3Denoise dfn3; MossFormerDenoise moss;
    cr.prepare (spec); lv.prepare (spec); pc.prepare (spec);
    ts.prepare (spec); tp.prepare (spec); ue.prepare (spec);
    dfn3.setModelPath (dfn3Model);
    dfn3.prepare (sr, spec.maximumBlockSize, buf.getNumChannels());
    moss.setModelPath (mossModel);
    moss.prepare (sr, spec.maximumBlockSize, buf.getNumChannels());
    classic.prepare (sr, spec.maximumBlockSize, buf.getNumChannels());

    auto chain = Presets::chainFor (preset, mode);
    if (denoiseAmountPct >= 0.0f)
        chain.noiseSuppressor.amount = juce::jlimit (0.0f, 1.0f, denoiseAmountPct / 100.0f);
    chain.toneShaper.tone = juce::jlimit (-1.0f, 1.0f, tone);
    if (! expanderOn)
        chain.upwardExpander.enabled = false;
    cr.setParams (chain.channelRepair);
    lv.setParams (chain.leveler);
    pc.setParams (chain.peakCompressor);
    ts.setParams (chain.toneShaper);
    ue.setParams (chain.upwardExpander);
    tp.setParams (chain.truePeakLimiter);
    classic.setClassicParams (chain.noiseSuppressor);

    DenoiseStage* stage = &classic;
    if (mode == DenoiseMode::live)    stage = &dfn3;
    else if (mode == DenoiseMode::hq) stage = &moss;

    const float amount = chain.noiseSuppressor.amount;
    dfn3.setAmount (amount);
    moss.setAmount (amount);

    // Continuous per-sample scene level (no output gain offline: base 0).
    pc.bindSceneSource (&lv.sceneStream());
    ue.bindSceneSource (&lv.sceneStream());

    const int block = 512;
    const int tapStop = tap == "denoise" ? 1 : 0;
    for (int off = 0; off < buf.getNumSamples(); off += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - off);
        juce::AudioBuffer<float> slice (buf.getNumChannels(), n);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, buf, ch, off, n);
        cr.process (slice);
        lv.process (slice);
        stage->process (slice);
        if (tapStop)
        {
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                buf.copyFrom (ch, off, slice, ch, 0, n);
            continue;
        }
        ts.process (slice);
        ue.setSceneLevelDb (0.0f);
        ue.process (slice);
        pc.setSceneLevelDb (0.0f);
        pc.process (slice);
        tp.process (slice);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.copyFrom (ch, off, slice, ch, 0, n);
    }
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: SyncTrackPrepOffline <in.wav> <out.wav> [soft|strong|clean] "
                     "[off|classic|live|hq] [amount 0-100|-1] [tone -1..1] "
                     "[--tap denoise|final] [--expander 0|1] [--flush <sec>] "
                     "[--dfn3 <tar.gz>] [--moss <onnx>]\n";
        return 1;
    }
    int preset = Presets::strong;
    DenoiseMode mode = Presets::denoiseModeDefault (preset);
    bool modeOk = true;
    float amount = -1.0f;
    float tone = 0.0f;
    juce::String tap = "final";
    bool expanderOn = true;
    double flushSec = 0.0;
    juce::File dfn3Model, mossModel;

    std::vector<juce::String> positional;
    for (int i = 3; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--tap")          tap = i + 1 < argc ? juce::String (argv[++i]) : tap;
        else if (a == "--expander") expanderOn = (i + 1 < argc ? juce::String (argv[++i]) : juce::String()) != "0";
        else if (a == "--flush")    flushSec = (i + 1 < argc ? juce::String (argv[++i]) : juce::String()).getDoubleValue();
        else if (a == "--dfn3")     dfn3Model = i + 1 < argc ? juce::String (argv[++i]) : juce::String();
        else if (a == "--moss")     mossModel = i + 1 < argc ? juce::String (argv[++i]) : juce::String();
        else positional.push_back (a);
    }
    auto positionalAs = [&] (int idx) -> const juce::String
    { return idx < positional.size() ? positional[(size_t) idx] : juce::String(); };

    if (positional.size() > 0)
    {
        const auto p = positionalAs (0);
        if (p == "soft") preset = Presets::soft;
        else if (p == "clean") preset = Presets::clean;
        else preset = Presets::strong;
    }
    if (positional.size() > 1)
        mode = parseMode (positionalAs (1).toRawUTF8(), modeOk);
    if (! modeOk) { std::cerr << "Bad mode (off|classic|live|hq)\n"; return 1; }
    if (positional.size() > 2)
        amount = (float) positionalAs (2).getDoubleValue();
    if (positional.size() > 3)
        tone = (float) positionalAs (3).getDoubleValue();
    // Sensible default model locations for local runs.
    if (dfn3Model == juce::File())
    {
        const auto p = juce::File::getCurrentWorkingDirectory()
                           .getChildFile ("third_party/dfn/model/DeepFilterNet3_onnx.tar.gz");
        if (p.existsAsFile())
            dfn3Model = p;
    }
    if (mossModel == juce::File())
    {
        const auto p = juce::File::getCurrentWorkingDirectory()
                           .getChildFile ("third_party/mossformer2/mossformer2_fp32.onnx");
        if (p.existsAsFile())
            mossModel = p;
    }

    juce::File inFile (argv[1]), outFile (argv[2]);
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inFile));
    if (reader == nullptr) { std::cerr << "Cannot open input\n"; return 1; }

    const int flushSamples = (int) std::ceil (flushSec * reader->sampleRate);
    juce::AudioBuffer<float> buf ((int) reader->numChannels,
                                  (int) reader->lengthInSamples + flushSamples);
    reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, true);
    if (buf.getNumChannels() < 2)
    {
        juce::AudioBuffer<float> st (2, buf.getNumSamples());
        st.copyFrom (0, 0, buf, 0, 0, buf.getNumSamples());
        st.copyFrom (1, 0, buf, 0, 0, buf.getNumSamples());
        buf = std::move (st);
    }

    applyChain (buf, reader->sampleRate, preset, mode, amount, tone, tap,
                expanderOn, dfn3Model, mossModel);

    if (outFile.existsAsFile())
        outFile.deleteFile();
    juce::WavAudioFormat wav;
    auto* fos = outFile.createOutputStream().release();
    if (fos == nullptr) { std::cerr << "Cannot write\n"; return 1; }
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (fos, reader->sampleRate, (unsigned int) buf.getNumChannels(), 24, {}, 0));
    if (writer == nullptr) { delete fos; return 1; }
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    std::cout << "Wrote " << argv[2] << " preset=" << preset
              << " mode=" << denoiseModeName ((int) mode)
              << " tap=" << tap << " expander=" << (expanderOn ? 1 : 0)
              << " flush=" << flushSec << "s"
              << " amount=" << amount << " tone=" << tone << "\n";
    return 0;
}
