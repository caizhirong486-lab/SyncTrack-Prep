// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

/** FabFilter-style 3D gold rotary look. */
class GoldKnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override;
};

class SyncTrackPrepEditor : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit SyncTrackPrepEditor (SyncTrackPrepProcessor&);
    ~SyncTrackPrepEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    SyncTrackPrepProcessor& proc;
    GoldKnobLookAndFeel goldLf;

    juce::Label titleLabel;
    juce::ComboBox presetBox;
    juce::Label presetLabel;
    juce::ToggleButton denoiseBtn { "Denoise" };
    juce::ToggleButton bypassBtn { "Bypass" };
    juce::Slider outputSlider;
    juce::Label outputLabel;
    juce::Label inMeterLabel;
    juce::Label outMeterLabel;

    float inPeakSmooth = 0.0f;
    float outPeakSmooth = 0.0f;
    juce::Rectangle<int> inMeterBounds, outMeterBounds;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<ComboAttachment> presetAtt;
    std::unique_ptr<ButtonAttachment> denoiseAtt, bypassAtt;
    std::unique_ptr<SliderAttachment> outputAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncTrackPrepEditor)
};
