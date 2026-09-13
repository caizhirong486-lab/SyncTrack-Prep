// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <JuceHeader.h>
#include "dsp/ChannelRepair.h"
#include "dsp/ClassicDenoise.h"
#include "dsp/Dfn3Denoise.h"
#include "dsp/MossFormerDenoise.h"
#include "dsp/MossFormerShortDenoise.h"
#include "dsp/HqInstanceLease.h"
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
    double getTailLengthSeconds() const override;
    /** The VST3 wrapper flips realtime/offline here (setupProcessing) without
        re-preparing when rate and block size are unchanged. Publish the
        engine's latency for the new mode synchronously so an offline HQ render
        never starts with a stale realtime latency. */
    void setNonRealtime (bool shouldBeNonRealtime) noexcept override;

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

    /** True while HQ runs degraded (capacity/deadline/model fallback) — the
        editor shows the specific reason. */
    bool isHqDegraded() const;

    /** Explicit HQ render target (plan: serialisable APVTS root property, not
        a host-automatable parameter). */
    enum class HqRenderTarget { shortMixdown = 0, dop4s = 1 };
    HqRenderTarget getHqRenderTarget() const { return hqRenderTarget; }
    void setHqRenderTarget (HqRenderTarget t);

private:
    HqRenderTarget readRenderTarget (const juce::ValueTree& state) const;

public:

    /** Aggregated HQ runtime state for the editor (short engine unless the
        4s DOP engine is the offline stage). */
    HqRuntimeState getHqRuntimeState() const;

    /** Editor polling: transport actually running (blocks target changes). */
    bool isTransportRunning() const { return transportPlaying.load (std::memory_order_relaxed); }

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
    int latencyForMode (DenoiseMode mode);
    DenoiseStage* stageForMode (DenoiseMode mode);

    ChannelRepair channelRepair;
    ClassicDenoise classicStage;   // Off (disabled dry delay) + Classic
    Dfn3Denoise dfn3Stage;         // Live (and the HQ alignment chain)
    MossFormerDenoise mossStage;   // HQ 4s DOP window (offline only)
    MossFormerShortDenoise mossShort; // HQ 160ms short window (realtime + Mixdown)
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

    HqRenderTarget hqRenderTarget = HqRenderTarget::shortMixdown;
    std::atomic<bool> hqDegraded { false };
    std::atomic<bool> transportPlaying { false };
    std::atomic<bool> dspPrepared { false };
    std::int64_t lastStreamTime = -1;
    std::uint64_t leaseId = 0;
    std::atomic<int> pendingLatency { -1 };
    std::atomic<float> inputPeak { 0.0f };
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<float> grDb { 0.0f };

    juce::AudioBuffer<float> dryBypass;

    // Forensics trace: inert unless /tmp/synctrackprep_trace.enable exists at
    // construction; appends to /tmp/synctrackprep_trace.log. Single-fwrite
    // lines keep interleaved instances readable without a lock.
    void tracef (const char* fmt, ...) const noexcept;
    static std::atomic<int> traceInstanceCounter;
    std::FILE* traceFile = nullptr;
    int traceInstance = 0;
    int traceBlockCount = 0;
    int traceBounceBlocks = 0;
    int traceLastWin = -1;
    int traceNonRtRepeats = 0;
    mutable int traceLatencyReads = 0;
    mutable int traceTailReads = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncTrackPrepProcessor)
};
