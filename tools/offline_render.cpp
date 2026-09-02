// SPDX-License-Identifier: AGPL-3.0-or-later
/**
 * Offline render — Round 2 presets.
 * Usage: SyncTrackPrepOffline <in.wav> <out.wav> [soft|strong|clean] [denoise 0|1]
 */
#include "dsp/Presets.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <cstring>

static void applyChain (juce::AudioBuffer<float>& buf, double sr, int preset, bool denoiseOn)
{
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, buf.getNumSamples()),
                                  (juce::uint32) juce::jmax (1, buf.getNumChannels()) };

    ChannelRepair cr; NoiseSuppressor ns; Leveler lv; PeakCompressor pc; TruePeakLimiter tp;
    cr.prepare (spec); ns.prepare (spec); lv.prepare (spec); pc.prepare (spec); tp.prepare (spec);

    const auto chain = Presets::chainFor (preset, denoiseOn);
    cr.setParams (chain.channelRepair);
    ns.setParams (chain.noiseSuppressor);
    lv.setParams (chain.leveler);
    pc.setParams (chain.peakCompressor);
    tp.setParams (chain.truePeakLimiter);

    const int block = 512;
    for (int off = 0; off < buf.getNumSamples(); off += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - off);
        juce::AudioBuffer<float> slice (buf.getNumChannels(), n);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, buf, ch, off, n);
        cr.process (slice); lv.process (slice); ns.process (slice); pc.process (slice); tp.process (slice);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.copyFrom (ch, off, slice, ch, 0, n);
    }
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: SyncTrackPrepOffline <in.wav> <out.wav> [soft|strong|clean] [denoise 0|1]\n";
        return 1;
    }
    int preset = Presets::strong;
    if (argc >= 4)
    {
        if (std::strcmp (argv[3], "soft") == 0) preset = Presets::soft;
        else if (std::strcmp (argv[3], "clean") == 0) preset = Presets::clean;
        else preset = Presets::strong;
    }
    bool denoise = Presets::denoiseDefault (preset);
    if (argc >= 5)
        denoise = std::atoi (argv[4]) != 0;

    juce::File inFile (argv[1]), outFile (argv[2]);
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inFile));
    if (reader == nullptr) { std::cerr << "Cannot open input\n"; return 1; }

    juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, true);
    if (buf.getNumChannels() < 2)
    {
        juce::AudioBuffer<float> st (2, buf.getNumSamples());
        st.copyFrom (0, 0, buf, 0, 0, buf.getNumSamples());
        st.copyFrom (1, 0, buf, 0, 0, buf.getNumSamples());
        buf = std::move (st);
    }

    applyChain (buf, reader->sampleRate, preset, denoise);

    if (outFile.existsAsFile())
        outFile.deleteFile();
    juce::WavAudioFormat wav;
    auto* fos = outFile.createOutputStream().release();
    if (fos == nullptr) { std::cerr << "Cannot write\n"; return 1; }
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (fos, reader->sampleRate, (unsigned int) buf.getNumChannels(), 24, {}, 0));
    if (writer == nullptr) { delete fos; return 1; }
    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    std::cout << "Wrote " << argv[2] << " preset=" << preset << " denoise=" << (denoise ? 1 : 0) << "\n";
    return 0;
}
