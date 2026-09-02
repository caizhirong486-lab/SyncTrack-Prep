// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginEditor.h"

void GoldKnobLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float rotaryStartAngle,
                                            float rotaryEndAngle, juce::Slider& slider)
{
    juce::ignoreUnused (slider);
    const float radius = (float) juce::jmin (width, height) * 0.5f - 6.0f;
    const float cx = (float) x + (float) width * 0.5f;
    const float cy = (float) y + (float) height * 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Soft drop shadow
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (cx - radius + 3.0f, cy - radius + 5.0f, radius * 2.0f, radius * 2.0f);

    // Outer bezel (dark metal ring)
    juce::ColourGradient bezel (juce::Colour (0xff3a3a3a), cx, cy - radius,
                                juce::Colour (0xff101010), cx, cy + radius, false);
    g.setGradientFill (bezel);
    g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    // Main gold body
    const float r2 = radius * 0.86f;
    juce::ColourGradient body (juce::Colour (0xffffe7a0), cx - r2 * 0.4f, cy - r2 * 0.7f,
                               juce::Colour (0xffb8860b), cx + r2 * 0.3f, cy + r2 * 0.8f, false);
    body.addColour (0.45, juce::Colour (0xfff0c14b));
    g.setGradientFill (body);
    g.fillEllipse (cx - r2, cy - r2, r2 * 2.0f, r2 * 2.0f);

    // Specular highlight cap
    juce::ColourGradient hi (juce::Colours::white.withAlpha (0.55f), cx, cy - r2 * 0.85f,
                             juce::Colours::transparentWhite, cx, cy, true);
    g.setGradientFill (hi);
    g.fillEllipse (cx - r2 * 0.75f, cy - r2 * 0.85f, r2 * 1.5f, r2 * 0.9f);

    // Inner rim
    g.setColour (juce::Colour (0xff6b4e12).withAlpha (0.65f));
    g.drawEllipse (cx - r2, cy - r2, r2 * 2.0f, r2 * 2.0f, 1.2f);

    // Pointer
    juce::Path p;
    const float pr = r2 * 0.72f;
    p.addRoundedRectangle (-2.0f, -pr, 4.0f, pr * 0.55f, 1.5f);
    g.setColour (juce::Colour (0xff2a1a00));
    g.fillPath (p, juce::AffineTransform::rotation (angle).translated (cx, cy));

    // Tick marks
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = rotaryStartAngle + (float) i / 10.0f * (rotaryEndAngle - rotaryStartAngle);
        const float x1 = cx + (radius - 2.0f) * std::cos (a - juce::MathConstants<float>::halfPi);
        const float y1 = cy + (radius - 2.0f) * std::sin (a - juce::MathConstants<float>::halfPi);
        // use angle from 12 o'clock: juce rotary uses different convention
        juce::ignoreUnused (x1, y1);
    }
}

SyncTrackPrepEditor::SyncTrackPrepEditor (SyncTrackPrepProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (320, 480);
    setLookAndFeel (&goldLf);

    titleLabel.setText ("SyncTrack Prep", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centred);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.92f));
    titleLabel.setFont (juce::FontOptions (18.0f).withStyle ("Bold"));
    addAndMakeVisible (titleLabel);

    presetLabel.setText ("Preset", juce::dontSendNotification);
    presetLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
    presetLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (presetLabel);

    presetBox.addItemList (juce::StringArray { "Soft", "Strong", "Clean" }, 1);
    presetBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1e222b));
    presetBox.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3a4050));
    presetBox.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    addAndMakeVisible (presetBox);

    auto styleToggle = [] (juce::ToggleButton& b)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::ToggleButton::textColourId, juce::Colours::white.withAlpha (0.9f));
        b.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xfff0c14b));
    };
    styleToggle (denoiseBtn);
    styleToggle (bypassBtn);
    addAndMakeVisible (denoiseBtn);
    addAndMakeVisible (bypassBtn);

    outputSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    outputSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 22);
    outputSlider.setRotaryParameters (juce::MathConstants<float>::pi * 1.15f,
                                      juce::MathConstants<float>::pi * 2.85f,
                                      true);
    outputSlider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    outputSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    outputSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (outputSlider);

    outputLabel.setText ("OUTPUT", juce::dontSendNotification);
    outputLabel.setJustificationType (juce::Justification::centred);
    outputLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));
    outputLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (outputLabel);

    inMeterLabel.setJustificationType (juce::Justification::centredLeft);
    outMeterLabel.setJustificationType (juce::Justification::centredLeft);
    inMeterLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    outMeterLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    inMeterLabel.setFont (juce::FontOptions (11.0f));
    outMeterLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (inMeterLabel);
    addAndMakeVisible (outMeterLabel);

    auto& ap = proc.apvts;
    presetAtt  = std::make_unique<ComboAttachment> (ap, "preset", presetBox);
    denoiseAtt = std::make_unique<ButtonAttachment> (ap, "denoise", denoiseBtn);
    bypassAtt  = std::make_unique<ButtonAttachment> (ap, "bypass", bypassBtn);
    outputAtt  = std::make_unique<SliderAttachment> (ap, "outputGain", outputSlider);

    startTimerHz (30);
}

SyncTrackPrepEditor::~SyncTrackPrepEditor()
{
    setLookAndFeel (nullptr);
    stopTimer();
}

void SyncTrackPrepEditor::paint (juce::Graphics& g)
{
    // Background
    juce::ColourGradient bg (juce::Colour (0xff1a1d24), 0, 0,
                             juce::Colour (0xff0e1014), 0, (float) getHeight(), false);
    g.setGradientFill (bg);
    g.fillAll();

    // Card behind knob
    auto card = juce::Rectangle<float> (24.0f, 100.0f, (float) getWidth() - 48.0f, 220.0f);
    g.setColour (juce::Colour (0xff161920));
    g.fillRoundedRectangle (card, 14.0f);
    g.setColour (juce::Colour (0xff2a303c));
    g.drawRoundedRectangle (card, 14.0f, 1.0f);

    // Meters
    auto drawBar = [&] (juce::Rectangle<int> area, float peak, const juce::String& name)
    {
        g.setColour (juce::Colour (0xff22262f));
        g.fillRoundedRectangle (area.toFloat(), 5.0f);
        const float db = juce::Decibels::gainToDecibels (peak, -60.0f);
        const float t = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        auto fill = area.reduced (3);
        const float w = (float) fill.getWidth() * t;
        juce::Colour c = t > 0.9f ? juce::Colour (0xffe85d5d) : juce::Colour (0xff4aa3d9);
        g.setColour (c);
        g.fillRoundedRectangle ((float) fill.getX(), (float) fill.getY(), w, (float) fill.getHeight(), 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.setFont (11.0f);
        g.drawText (name + "  " + juce::String (db, 1), area.reduced (6, 0), juce::Justification::centredLeft);
    };

    if (! inMeterBounds.isEmpty())
        drawBar (inMeterBounds, inPeakSmooth, "IN");
    if (! outMeterBounds.isEmpty())
        drawBar (outMeterBounds, outPeakSmooth, "OUT");
}

void SyncTrackPrepEditor::resized()
{
    auto r = getLocalBounds().reduced (20);

    titleLabel.setBounds (r.removeFromTop (28));
    r.removeFromTop (14);

    // Row: Preset label + box | Denoise
    auto row1 = r.removeFromTop (28);
    presetLabel.setBounds (row1.removeFromLeft (48));
    presetBox.setBounds (row1.removeFromLeft (120));
    row1.removeFromLeft (12);
    denoiseBtn.setBounds (row1.removeFromLeft (100).withSizeKeepingCentre (100, 28));

    r.removeFromTop (18);

    // Knob zone (card interior)
    auto knobZone = r.removeFromTop (200);
    outputSlider.setBounds (knobZone.withSizeKeepingCentre (168, 168));
    r.removeFromTop (4);
    outputLabel.setBounds (r.removeFromTop (18));

    r.removeFromTop (16);

    // Meters stacked
    inMeterBounds = r.removeFromTop (26);
    r.removeFromTop (8);
    outMeterBounds = r.removeFromTop (26);

    r.removeFromTop (16);
    bypassBtn.setBounds (r.removeFromTop (32).withSizeKeepingCentre (110, 32));
}

void SyncTrackPrepEditor::timerCallback()
{
    const float in = proc.getInputPeak();
    const float out = proc.getOutputPeak();
    inPeakSmooth = juce::jmax (in, 0.82f * inPeakSmooth + 0.18f * in);
    outPeakSmooth = juce::jmax (out, 0.82f * outPeakSmooth + 0.18f * out);
    if (in < inPeakSmooth * 0.99f)
        inPeakSmooth *= 0.94f;
    if (out < outPeakSmooth * 0.99f)
        outPeakSmooth *= 0.94f;

    inMeterLabel.setText ({}, juce::dontSendNotification);
    outMeterLabel.setText ({}, juce::dontSendNotification);
    repaint (inMeterBounds.expanded (2).getUnion (outMeterBounds.expanded (2)));
}
