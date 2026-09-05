// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginProcessor.h"
#include "PluginEditor.h"

SyncTrackPrepProcessor::SyncTrackPrepProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    pPreset        = apvts.getRawParameterValue ("preset");
    pDenoise       = apvts.getRawParameterValue ("denoise");
    pDenoiseAmount = apvts.getRawParameterValue ("denoiseAmount");
    pTone          = apvts.getRawParameterValue ("tone");
    pOutputGain    = apvts.getRawParameterValue ("outputGain");
    pBypass        = apvts.getRawParameterValue ("bypass");

    apvts.addParameterListener ("preset", this);
    applyPresetDefaults (1); // Strong default
}

SyncTrackPrepProcessor::~SyncTrackPrepProcessor()
{
    apvts.removeParameterListener ("preset", this);
}

juce::AudioProcessorValueTreeState::ParameterLayout SyncTrackPrepProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    juce::StringArray presetNames;
    for (int i = 0; i < Presets::count; ++i)
        presetNames.add (Presets::name (i));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "preset", 1 }, "Preset", presetNames, Presets::strong));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "denoise", 1 }, "Denoise", Presets::denoiseDefault (Presets::strong)));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "denoiseAmount", 1 }, "Amount",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 1.0f },
        (float) Presets::denoiseAmountDefault (Presets::strong)));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tone", 1 }, "Tone",
        juce::NormalisableRange<float> { -1.0f, 1.0f, 0.01f }, 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "outputGain", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 12.0f, 0.1f }, 0.0f));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return { params.begin(), params.end() };
}

void SyncTrackPrepProcessor::parameterChanged (const juce::String& parameterID, float)
{
    // During setStateInformation the APVTS restores children in tree order and
    // notifies each one, so the restored "preset" would rewrite the user's saved
    // denoise choice. Preset defaults must only follow a real preset gesture.
    if (isLoadingState)
        return;

    if (parameterID == "preset")
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("preset")))
        {
            const int idx = p->getIndex();
            // Re-selecting the same preset is not a gesture: it must not reset
            // a denoise choice the user made since the last switch.
            if (idx != lastPreset)
                applyPresetDefaults (idx);
        }
    }
}

void SyncTrackPrepProcessor::applyPresetDefaults (int presetIndex)
{
    const bool denoiseOn = Presets::denoiseDefault (presetIndex);
    if (auto* p = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("denoise")))
    {
        const bool cur = p->get();
        if (cur != denoiseOn)
            p->setValueNotifyingHost (denoiseOn ? 1.0f : 0.0f);
    }
    if (auto* p = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("denoiseAmount")))
    {
        const float cur = p->get();
        const float def = (float) Presets::denoiseAmountDefault (presetIndex);
        if (std::abs (cur - def) > 1.0e-4f)
            p->setValueNotifyingHost (p->convertTo0to1 (def));
    }
    lastPreset = presetIndex;
}

void SyncTrackPrepProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec {
        sampleRate,
        (juce::uint32) juce::jmax (1, samplesPerBlock),
        (juce::uint32) juce::jmax (1, getTotalNumOutputChannels())
    };

    channelRepair.prepare (spec);
    noiseSuppressor.prepare (spec);
    leveler.prepare (spec);
    peakCompressor.prepare (spec);
    toneShaper.prepare (spec);
    truePeakLimiter.prepare (spec);

    dryBypass.setSize (2, samplesPerBlock, false, true, true);

    setLatencySamples (noiseSuppressor.getLatencySamples() + truePeakLimiter.getLatencySamples());
}

void SyncTrackPrepProcessor::releaseResources() {}

bool SyncTrackPrepProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void SyncTrackPrepProcessor::updateDspParams()
{
    const int preset = juce::jlimit (0, 2, juce::roundToInt (pPreset != nullptr ? pPreset->load() : 1.0f));
    const bool denoiseOn = pDenoise != nullptr && pDenoise->load() >= 0.5f;

    auto chain = Presets::chainFor (preset, denoiseOn);
    // The Amount knob and Tone knob own these two; the preset only seeds them.
    if (pDenoiseAmount != nullptr)
        chain.noiseSuppressor.amount = juce::jlimit (0.0f, 1.0f, pDenoiseAmount->load() / 100.0f);
    if (pTone != nullptr)
        chain.toneShaper.tone = juce::jlimit (-1.0f, 1.0f, pTone->load());
    channelRepair.setParams (chain.channelRepair);
    noiseSuppressor.setParams (chain.noiseSuppressor);
    leveler.setParams (chain.leveler);
    peakCompressor.setParams (chain.peakCompressor);
    toneShaper.setParams (chain.toneShaper);
    truePeakLimiter.setParams (chain.truePeakLimiter);
}

void SyncTrackPrepProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    float inPeak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        inPeak = juce::jmax (inPeak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
    inputPeak.store (inPeak, std::memory_order_relaxed);

    const bool bypass = pBypass != nullptr && pBypass->load() >= 0.5f;
    if (bypass)
    {
        outputPeak.store (inPeak, std::memory_order_relaxed);
        grDb.store (0.0f, std::memory_order_relaxed);
        return;
    }

    updateDspParams();

    // Level before denoise so spectral stage can shave boosted floor (Clean)
    channelRepair.process (buffer);
    leveler.process (buffer);
    noiseSuppressor.process (buffer);
    toneShaper.process (buffer);

    // Output is the chain's makeup gain, ahead of the always-on safety stages:
    // the -1 dBTP ceiling holds at any knob position. The
    // compressor acts on the boosted signal, and the limiter clamps whatever
    // the knob adds beyond it — so +12 dB adds almost no loudness.
    const float outGdB = pOutputGain != nullptr ? pOutputGain->load() : 0.0f;
    buffer.applyGain (juce::Decibels::decibelsToGain (outGdB));

    // The compressor references the post-leveler scene level (the leveler
    // normalizes it near -18 dB) plus the output gain, so threshold tracking
    // survives the makeup gain instead of drifting with the knob.
    peakCompressor.setSceneLevelDb (leveler.getSlowEnvDb() + outGdB);
    peakCompressor.process (buffer);
    truePeakLimiter.process (buffer);

    float outPeak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        outPeak = juce::jmax (outPeak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
    outputPeak.store (outPeak, std::memory_order_relaxed);
    grDb.store (peakCompressor.getLastGainReductionDb(), std::memory_order_relaxed);
}

void SyncTrackPrepProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void SyncTrackPrepProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            const juce::ScopedValueSetter<bool> loading (isLoadingState, true);
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            lastPreset = juce::roundToInt (pPreset != nullptr ? pPreset->load() : 1.0f);
        }
    }
}

juce::AudioProcessorEditor* SyncTrackPrepProcessor::createEditor()
{
    return new SyncTrackPrepEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SyncTrackPrepProcessor();
}
