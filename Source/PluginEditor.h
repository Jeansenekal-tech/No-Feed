#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class DeFeedbackLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DeFeedbackLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
};

/** Live input spectrum with the deployed notches and their combined response on top. */
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (const dfb::FeedbackEngine& e);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    float xForFreq (float f) const;
    float yForDb (float db) const;

    const dfb::FeedbackEngine& engine;
    std::array<float, dfb::FeedbackEngine::kDisplayPoints> spectrum {};
    std::array<dfb::FeedbackEngine::NotchSnapshot, dfb::FeedbackEngine::kMaxNotches> notches {};
};

class DeFeedbackEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DeFeedbackEditor (DeFeedbackProcessor&);
    ~DeFeedbackEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void timerCallback() override;
    void addKnob (Knob&, const char* paramId, const juce::String& name, std::vector<Knob*>& row);
    void layoutRow (juce::Rectangle<int> area, std::vector<Knob*>& row, int knobWidth);

    DeFeedbackProcessor& processor;
    DeFeedbackLookAndFeel lnf;
    SpectrumView spectrum;

    Knob sensitivity, speed, release, maxDepth, width, filters, lowHz, highHz;
    Knob deNoise, deEss, deEssFreq, mix, output;
    std::vector<Knob*> feedbackRow, cleanupRow, outputRow;
    juce::Rectangle<int> feedbackHeader, cleanupHeader, outputHeader;

    juce::TextButton lockButton { "LOCK" }, listenButton { "LISTEN" }, resetButton { "RESET" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lockAttachment, listenAttachment;
    juce::Label status, cleanupStatus;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeFeedbackEditor)
};
