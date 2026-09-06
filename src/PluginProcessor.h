// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <JuceHeader.h>
#include "dsp/ChannelRepair.h"
#include "dsp/ClassicDenoise.h"
#include "dsp/Dfn3Denoise.h"
#include "dsp/MossFormerDenoise.h"
#include "dsp/Leveler.h"
#include "dsp/PeakCompressor.h"
#include "dsp/ToneShaper.h"
#include "dsp/UpwardExpander.h"
#include "dsp/TruePeakLimiter.h"
#include "dsp/Presets.h"

class SyncTrackPrepProcessor : public juce::AudioProcessor,
                               private juce::AudioProcessorValueTreeState::Listener,
                               private juce::AsyncUpdater
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

    /** True while the user picked HQ but the host is running realtime and the
        engine was downgraded to Live (DFN3) — the editor shows a hint. */
    bool isHqDegraded() const { return hqDegraded.load (std::memory_order_relaxed); }

    /** Applies a pending latency change immediately (message thread only).
        The host normally gets it via the AsyncUpdater; headless tests have no
        message loop and call this directly. */
    void flushPendingLatency() { handleUpdateNowIfNeeded(); }

    /** Bundle resource lookup: <exe>/Resources/<sub>, <exe>/<sub>, the bundle
        Resources two levels up, or the working directory. */
    static juce::File findBundleResourceDir (const juce::String& sub);

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void updateDspParams();
    void applyPresetDefaults (int presetIndex);
    /** Publishes the pending latency on the message thread: setLatencySamples
        makes the host restart the component, which must never happen from
        processBlock. */
    void handleAsyncUpdate() override;
    void requestLatencyUpdate (int samples);
    DenoiseStage* stageForMode (DenoiseMode mode);

    ChannelRepair channelRepair;
    ClassicDenoise classicStage;   // Off (disabled dry delay) + Classic
    Dfn3Denoise dfn3Stage;         // Live (and realtime-degraded HQ)
    MossFormerDenoise mossStage;   // HQ (non-realtime only)
    Leveler leveler;
    PeakCompressor peakCompressor;
    ToneShaper toneShaper;
    UpwardExpander upwardExpander;
    TruePeakLimiter truePeakLimiter;

    DenoiseStage* activeStage = nullptr;
    DenoiseStage* prevStage = nullptr;   // outgoing engine during a crossfade
    juce::AudioBuffer<float> fadeTmp;
    int fadeLen = 720;                   // 15 ms equal-power crossfade
    int fadePos = 0;

    std::atomic<float>* pPreset = nullptr;
    std::atomic<float>* pDenoiseMode = nullptr;
    std::atomic<float>* pDenoise = nullptr;
    std::atomic<float>* pDenoiseAmount = nullptr;
    std::atomic<float>* pTone = nullptr;
    std::atomic<float>* pOutputGain = nullptr;
    std::atomic<float>* pBypass = nullptr;

    int lastPreset = -1;
    /** Set while setStateInformation runs, so restoring a preset does not
        overwrite the saved denoise value. */
    bool isLoadingState = false;

    std::atomic<bool> hqDegraded { false };
    std::atomic<int> pendingLatency { -1 };
    std::atomic<float> inputPeak { 0.0f };
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<float> grDb { 0.0f };

    juce::AudioBuffer<float> dryBypass;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncTrackPrepProcessor)
};
