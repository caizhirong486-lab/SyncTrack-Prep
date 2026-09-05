// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <JuceHeader.h>
#include "dsp/ChannelRepair.h"
#include "dsp/NoiseSuppressor.h"
#include "dsp/Leveler.h"
#include "dsp/PeakCompressor.h"
#include "dsp/ToneShaper.h"
#include "dsp/TruePeakLimiter.h"
#include "dsp/Presets.h"

class SyncTrackPrepProcessor : public juce::AudioProcessor,
                               private juce::AudioProcessorValueTreeState::Listener
{
public:
    SyncTrackPrepProcessor();
    ~SyncTrackPrepProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void parameterChanged (const juce::String& parameterID, float newValue) override;

    juce::AudioProcessorValueTreeState apvts;

    float getInputPeak() const { return inputPeak.load (std::memory_order_relaxed); }
    float getOutputPeak() const { return outputPeak.load (std::memory_order_relaxed); }
    float getGrDb() const { return grDb.load (std::memory_order_relaxed); }

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void updateDspParams();
    void applyPresetDefaults (int presetIndex);

    ChannelRepair channelRepair;
    NoiseSuppressor noiseSuppressor;
    Leveler leveler;
    PeakCompressor peakCompressor;
    ToneShaper toneShaper;
    TruePeakLimiter truePeakLimiter;

    std::atomic<float>* pPreset = nullptr;
    std::atomic<float>* pDenoise = nullptr;
    std::atomic<float>* pDenoiseAmount = nullptr;
    std::atomic<float>* pTone = nullptr;
    std::atomic<float>* pOutputGain = nullptr;
    std::atomic<float>* pBypass = nullptr;

    int lastPreset = -1;
    /** Set while setStateInformation runs, so restoring a preset does not
        overwrite the saved denoise value. */
    bool isLoadingState = false;

    std::atomic<float> inputPeak { 0.0f };
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<float> grDb { 0.0f };

    juce::AudioBuffer<float> dryBypass;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncTrackPrepProcessor)
};
