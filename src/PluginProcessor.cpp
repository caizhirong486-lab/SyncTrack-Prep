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
    pDenoiseMode   = apvts.getRawParameterValue ("denoiseMode");
    pDenoise       = apvts.getRawParameterValue ("denoise");
    pDenoiseAmount = apvts.getRawParameterValue ("denoiseAmount");
    pTone          = apvts.getRawParameterValue ("tone");
    pOutputGain    = apvts.getRawParameterValue ("outputGain");
    pBypass        = apvts.getRawParameterValue ("bypass");

    apvts.addParameterListener ("preset", this);
    apvts.addParameterListener ("denoiseMode", this);
    applyPresetDefaults (1); // Strong default
}

SyncTrackPrepProcessor::~SyncTrackPrepProcessor()
{
    apvts.removeParameterListener ("preset", this);
    apvts.removeParameterListener ("denoiseMode", this);
}

juce::AudioProcessorValueTreeState::ParameterLayout SyncTrackPrepProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    juce::StringArray presetNames;
    for (int i = 0; i < Presets::count; ++i)
        presetNames.add (Presets::name (i));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "preset", 1 }, "Preset", presetNames, Presets::strong));

    juce::StringArray modeNames;
    for (int m = 0; m < numDenoiseModes; ++m)
        modeNames.add (denoiseModeName (m));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "denoiseMode", 1 }, "Denoise Mode", modeNames,
        (int) Presets::denoiseModeDefault (Presets::strong)));

    // Legacy denoise checkbox: kept for state compatibility. New states mirror
    // denoiseMode into it (on for Classic/Live/HQ, off for Off); loads from
    // states without denoiseMode map on->Live, off->Off (documented upgrade
    // mapping, see changelog).
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "denoise", 1 }, "Denoise", Presets::denoiseDefault (Presets::strong)));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "denoiseAmount", 1 }, "Amount",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 1.0f },
        (float) Presets::denoiseAmountDefault (Presets::strong)));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tone", 1 }, "Tone",
        juce::NormalisableRange<float> { -1.0f, 1.0f, 0.01f }, 0.0f));

    // 0 dB sits at the centre of the normalized range so the knob reads
    // symmetric; the range itself is asymmetric. Existing sessions stored
    // 0 dB at 0.667 (old -24..+12 range) and will load shifted — accepted
    // breakage, recorded in the changelog.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "outputGain", 1 }, "Output",
        juce::NormalisableRange<float> { -80.0f, 24.0f, 0.1f, 2.637f }, 0.0f));

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
    else if (parameterID == "denoiseMode")
    {
        // Keep the legacy bool mirroring the mode for saved-state coherence.
        if (auto* m = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("denoiseMode")))
        {
            const bool on = m->getIndex() != (int) DenoiseMode::off;
            if (auto* b = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("denoise")))
                if (b->get() != on)
                    b->setValueNotifyingHost (on ? 1.0f : 0.0f);
        }
    }
}

void SyncTrackPrepProcessor::applyPresetDefaults (int presetIndex)
{
    const DenoiseMode mode = Presets::denoiseModeDefault (presetIndex);
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("denoiseMode")))
    {
        const int cur = p->getIndex();
        if (cur != (int) mode)
            p->setValueNotifyingHost (p->convertTo0to1 ((float) mode));
    }
    if (auto* p = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("denoise")))
    {
        const bool on = mode != DenoiseMode::off;
        if (p->get() != on)
            p->setValueNotifyingHost (on ? 1.0f : 0.0f);
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

juce::File SyncTrackPrepProcessor::findBundleResourceDir (const juce::String& sub)
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto exeDir = exe.getParentDirectory();
    const juce::File candidates[] = {
        exeDir.getChildFile ("Resources").getChildFile (sub),
        exeDir.getChildFile (sub),
        exeDir.getParentDirectory().getChildFile ("Resources").getChildFile (sub),
        exeDir.getParentDirectory().getParentDirectory().getChildFile ("Resources").getChildFile (sub),
        juce::File::getCurrentWorkingDirectory().getChildFile (sub),
    };
    for (const auto& c : candidates)
        if (c.exists())
            return c;
    return {};
}

void SyncTrackPrepProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int numCh = juce::jmax (1, getTotalNumOutputChannels());
    juce::dsp::ProcessSpec spec {
        sampleRate,
        (juce::uint32) juce::jmax (1, samplesPerBlock),
        (juce::uint32) numCh
    };

    channelRepair.prepare (spec);
    leveler.prepare (spec);
    peakCompressor.prepare (spec);
    toneShaper.prepare (spec);
    truePeakLimiter.prepare (spec);
    upwardExpander.prepare (spec);

    // Continuous scene level: the leveler publishes its post-gain scene every
    // sample; the expander and compressor add their own offsets on top. The
    // per-block setSceneLevelDb below only carries the output makeup gain.
    peakCompressor.bindSceneSource (&leveler.sceneStream());
    upwardExpander.bindSceneSource (&leveler.sceneStream());

    const auto dfnModel = findBundleResourceDir ("dfn3").getChildFile ("DeepFilterNet3_onnx.tar.gz");
    dfn3Stage.setModelPath (dfnModel);
    dfn3Stage.prepare (sampleRate, samplesPerBlock, numCh);

    const auto mossModel = findBundleResourceDir ("mossformer2").getChildFile ("mossformer2_fp32.onnx");
    mossStage.setModelPath (mossModel);
    mossStage.prepare (sampleRate, samplesPerBlock, numCh);

    classicStage.prepare (sampleRate, samplesPerBlock, numCh);

    fadeTmp.setSize (juce::jmax (1, numCh), juce::jmax (1, samplesPerBlock),
                     false, true, true);
    fadeLen = juce::jmax (64, (int) std::round (0.015 * sampleRate));
    fadePos = 0;
    prevStage = nullptr;

    // Resolve the engine here, on the message thread: reporting the classic
    // latency now and correcting it from the first processBlock would make the
    // host restart the component mid-render.
    const int modeIdx = juce::jlimit (0, numDenoiseModes - 1,
                                      juce::roundToInt (pDenoiseMode != nullptr ? pDenoiseMode->load() : 0.0f));
    DenoiseMode mode = static_cast<DenoiseMode> (modeIdx);
    if (mode == DenoiseMode::hq && ! isNonRealtime())
        mode = DenoiseMode::live;
    activeStage = stageForMode (mode);

    dryBypass.setSize (2, samplesPerBlock, false, true, true);

    const int latency = activeStage->getLatencySamples() + truePeakLimiter.getLatencySamples();
    pendingLatency.store (latency, std::memory_order_relaxed);
    setLatencySamples (latency);
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

DenoiseStage* SyncTrackPrepProcessor::stageForMode (DenoiseMode mode)
{
    switch (mode)
    {
        case DenoiseMode::live: return &dfn3Stage;
        case DenoiseMode::hq:   return &mossStage;
        case DenoiseMode::off:
        case DenoiseMode::classic:
        default:                return &classicStage;
    }
}

void SyncTrackPrepProcessor::requestLatencyUpdate (int samples)
{
    if (pendingLatency.exchange (samples, std::memory_order_relaxed) != samples)
        triggerAsyncUpdate();
}

void SyncTrackPrepProcessor::handleAsyncUpdate()
{
    const int samples = pendingLatency.load (std::memory_order_relaxed);
    if (samples >= 0)
        setLatencySamples (samples);
}

void SyncTrackPrepProcessor::updateDspParams()
{
    const int preset = juce::jlimit (0, 2, juce::roundToInt (pPreset != nullptr ? pPreset->load() : 1.0f));
    const int modeIdx = juce::jlimit (0, numDenoiseModes - 1,
                                      juce::roundToInt (pDenoiseMode != nullptr ? pDenoiseMode->load() : 0.0f));
    DenoiseMode mode = static_cast<DenoiseMode> (modeIdx);

    // HQ runs the heavy model only in offline (non-realtime) processing;
    // realtime playback of an HQ selection degrades to Live (DFN3).
    bool degraded = false;
    if (mode == DenoiseMode::hq && ! isNonRealtime())
    {
        mode = DenoiseMode::live;
        degraded = true;
    }
    hqDegraded.store (degraded, std::memory_order_relaxed);

    auto chain = Presets::chainFor (preset, mode);
    // The Amount knob and Tone knob own these two; the preset only seeds them.
    const float amount = pDenoiseAmount != nullptr
        ? juce::jlimit (0.0f, 1.0f, pDenoiseAmount->load() / 100.0f) : 0.45f;
    chain.noiseSuppressor.amount = amount;
    if (pTone != nullptr)
        chain.toneShaper.tone = juce::jlimit (-1.0f, 1.0f, pTone->load());
    classicStage.setClassicParams (chain.noiseSuppressor);
    dfn3Stage.setAmount (amount);
    mossStage.setAmount (amount);
    channelRepair.setParams (chain.channelRepair);
    leveler.setParams (chain.leveler);
    peakCompressor.setParams (chain.peakCompressor);
    toneShaper.setParams (chain.toneShaper);
    upwardExpander.setParams (chain.upwardExpander);
    truePeakLimiter.setParams (chain.truePeakLimiter);

    // Stage routing with a short crossfade between distinct engines.
    DenoiseStage* want = stageForMode (mode);
    if (activeStage == nullptr)
    {
        activeStage = want;
    }
    else if (want != activeStage && prevStage == nullptr)
    {
        // Only crossfade out of an engine that may run against a realtime
        // deadline; the HQ engine would run a 4 s ONNX window right here.
        prevStage = (isNonRealtime() || activeStage->supportsRealtime())
                        ? activeStage : nullptr;
        activeStage = want;
        fadePos = 0;
    }
    if (activeStage != nullptr)
    {
        // Hosts may ignore a mid-playback change (R5, changelog); the update
        // itself is deferred to the message thread.
        requestLatencyUpdate (activeStage->getLatencySamples()
                              + truePeakLimiter.getLatencySamples());
    }
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

    // Denoise stage: short equal-power crossfade between the outgoing and the
    // incoming engine when the mode changes.
    if (prevStage != nullptr)
    {
        // Match the host's actual block length: feeding the outgoing engine a
        // zero-padded fixed-size buffer would desync its internal FIFOs, and a
        // longer-than-prepared block would overrun fadeTmp.
        const int fadeN = juce::jmin (buffer.getNumSamples(), fadeTmp.getNumSamples());
        juce::AudioBuffer<float> fadeView (fadeTmp.getArrayOfWritePointers(),
                                           juce::jmin (buffer.getNumChannels(), fadeTmp.getNumChannels()),
                                           fadeN);
        fadeView.clear();
        for (int ch = 0; ch < fadeView.getNumChannels(); ++ch)
            fadeView.copyFrom (ch, 0, buffer, ch, 0, fadeN);
        prevStage->process (fadeView);
        if (activeStage != nullptr)
            activeStage->process (buffer);
        const int n = fadeN;
        const float total = (float) fadeLen;
        for (int i = 0; i < n; ++i)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (float) (fadePos + i) / total);
            const float gOld = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * t));
            const float gNew = 1.0f - gOld;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float mixed = fadeTmp.getSample (ch, i) * gOld
                                    + buffer.getSample (ch, i) * gNew;
                buffer.setSample (ch, i, mixed);
            }
        }
        fadePos += n;
        if (fadePos >= fadeLen)
            prevStage = nullptr;
    }
    else if (activeStage != nullptr)
    {
        activeStage->process (buffer);
    }

    toneShaper.process (buffer);

    // Output is the chain's makeup gain, ahead of the always-on safety stages:
    // the -1 dBTP ceiling holds at any knob position.
    const float outGdB = pOutputGain != nullptr ? pOutputGain->load() : 0.0f;
    buffer.applyGain (juce::Decibels::decibelsToGain (outGdB));

    // The expander and the compressor track the leveler's per-sample scene
    // stream (bound in prepareToPlay); only the output makeup gain rides in
    // per block, so their thresholds follow the knob without drifting.
    upwardExpander.setSceneLevelDb (outGdB);
    upwardExpander.process (buffer);
    peakCompressor.setSceneLevelDb (outGdB);
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
            juce::ValueTree tree = juce::ValueTree::fromXml (*xml);

            // Key precedence: states that carry denoiseMode are trusted as-is
            // (protects e.g. a saved Classic session from the legacy mapping);
            // only true legacy states (bool only) get the documented upgrade
            // mapping on->Live, off->Off. Decided BEFORE replaceState: APVTS
            // re-adds missing parameter children with defaults into the shared
            // tree, so a post-replace check would always see the key.
            const bool hasMode = tree.getChildWithProperty ("id", "denoiseMode").isValid();

            apvts.replaceState (tree);
            lastPreset = juce::roundToInt (pPreset != nullptr ? pPreset->load() : 1.0f);

            if (! hasMode)
            {
                const bool on = pDenoise != nullptr && pDenoise->load() >= 0.5f;
                if (auto* m = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("denoiseMode")))
                    m->setValueNotifyingHost (m->convertTo0to1 (
                        (float) (on ? DenoiseMode::live : DenoiseMode::off)));
            }
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
