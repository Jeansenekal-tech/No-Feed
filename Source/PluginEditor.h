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

/** Input spectrum (grey), AI output (cyan) and, with Notch Assist on, the notch response (red). */
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (const DeFeedbackProcessor& p);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    float xForFreq (float f) const;
    float yForDb (float db) const;
    juce::Path makePath (const float* db, int n, bool closed) const;

    const DeFeedbackProcessor& processor;
    std::array<float, dfb::AiVoiceEngine::kDisplayPoints> inDb {}, outDb {};
    std::array<dfb::FeedbackEngine::NotchSnapshot, dfb::FeedbackEngine::kMaxNotches> notches {};
    bool showNotches = false;
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

    Knob amount, deEss, deEssFreq, output;
    Knob sensitivity, speed, release, maxDepth, width, filters, lowHz, highHz;
    std::vector<Knob*> mainRow, notchRow;
    juce::Rectangle<int> amountArea, mainHeader, notchHeader;

    juce::TextButton listenButton { "LISTEN" }, notchButton { "NOTCH ASSIST" }, lockButton { "LOCK" }, resetButton { "RESET" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> listenAttachment, notchAttachment, lockAttachment;
    juce::Label removedLabel, status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeFeedbackEditor)
};
