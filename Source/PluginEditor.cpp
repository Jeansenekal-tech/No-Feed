#include "PluginEditor.h"

namespace Colours
{
    const juce::Colour background { 0xff12151b };
    const juce::Colour panel      { 0xff1a1f27 };
    const juce::Colour grid       { 0xff2a313c };
    const juce::Colour text       { 0xffd7dde6 };
    const juce::Colour dim        { 0xff7d8794 };
    const juce::Colour accent     { 0xff3fc1c9 };
    const juce::Colour notch      { 0xffff5a5f };
}

//==============================================================================
DeFeedbackLookAndFeel::DeFeedbackLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Colours::dim);
    setColour (juce::TextButton::buttonColourId, Colours::panel);
    setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    setColour (juce::TextButton::textColourOffId, Colours::text);
    setColour (juce::TextButton::textColourOnId, Colours::background);
}

void DeFeedbackLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                              float startAngle, float endAngle, juce::Slider&)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (6.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float lineW = 3.5f;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius - lineW, radius - lineW, 0.0f, startAngle, endAngle, true);
    g.setColour (Colours::grid);
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, radius - lineW, radius - lineW, 0.0f, startAngle, angle, true);
    g.setColour (Colours::accent);
    g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (Colours::panel.brighter (0.15f));
    g.fillEllipse (juce::Rectangle<float> (radius * 1.2f, radius * 1.2f).withCentre (centre));

    const auto tip = centre.getPointOnCircumference (radius * 0.5f, angle);
    g.setColour (Colours::text);
    g.drawLine ({ centre.getPointOnCircumference (radius * 0.15f, angle), tip }, 2.5f);
}

void DeFeedbackLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    auto c = b.getToggleState() ? Colours::accent : Colours::panel.brighter (0.1f);
    if (highlighted) c = c.brighter (0.1f);
    if (down)        c = c.darker (0.2f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (Colours::grid.brighter (0.2f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

//==============================================================================
SpectrumView::SpectrumView (const dfb::FeedbackEngine& e) : engine (e)
{
    spectrum.fill (-120.0f);
    startTimerHz (30);
}

void SpectrumView::timerCallback()
{
    engine.getDisplaySpectrum (spectrum.data(), (int) spectrum.size());
    notches = engine.getNotches();
    repaint();
}

float SpectrumView::xForFreq (float f) const
{
    const float lo = dfb::FeedbackEngine::kDisplayMinHz, hi = dfb::FeedbackEngine::kDisplayMaxHz;
    return (float) getWidth() * std::log (f / lo) / std::log (hi / lo);
}

float SpectrumView::yForDb (float db) const
{
    constexpr float top = 0.0f, bottom = -96.0f;
    return juce::jmap (juce::jlimit (bottom, top, db), top, bottom, 0.0f, (float) getHeight());
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (area, 6.0f);

    // grid
    g.setFont (juce::FontOptions (11.0f));
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForFreq (f);
        g.setColour (Colours::grid);
        g.drawVerticalLine ((int) x, 0.0f, area.getHeight());
        g.setColour (Colours::dim);
        g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                    (int) x + 3, getHeight() - 16, 40, 14, juce::Justification::left);
    }
    for (float db = -12.0f; db > -96.0f; db -= 12.0f)
    {
        const float y = yForDb (db);
        g.setColour (Colours::grid);
        g.drawHorizontalLine ((int) y, 0.0f, area.getWidth());
        g.setColour (Colours::dim);
        g.drawText (juce::String ((int) db), 4, (int) y - 13, 40, 12, juce::Justification::left);
    }

    // spectrum
    juce::Path fill, line;
    const int n = (int) spectrum.size();
    for (int i = 0; i < n; ++i)
    {
        const float x = area.getWidth() * (float) i / (float) (n - 1);
        const float y = yForDb (spectrum[(size_t) i]);
        if (i == 0) { line.startNewSubPath (x, y); fill.startNewSubPath (x, area.getHeight()); }
        else          line.lineTo (x, y);
        fill.lineTo (x, y);
    }
    fill.lineTo (area.getWidth(), area.getHeight());
    fill.closeSubPath();

    g.setGradientFill (juce::ColourGradient (Colours::accent.withAlpha (0.35f), 0.0f, 0.0f,
                                             Colours::accent.withAlpha (0.03f), 0.0f, area.getHeight(), false));
    g.fillPath (fill);
    g.setColour (Colours::accent);
    g.strokePath (line, juce::PathStrokeType (1.5f));

    // combined notch response, drawn hanging from the top of the display (0 dB line)
    juce::Path response;
    bool anyActive = false;
    const float scale = 1.0f;   // 1 dB of cut = 1 dB on the display
    for (int px = 0; px <= getWidth(); px += 2)
    {
        const float f = dfb::FeedbackEngine::kDisplayMinHz
                      * std::pow (dfb::FeedbackEngine::kDisplayMaxHz / dfb::FeedbackEngine::kDisplayMinHz, (float) px / (float) getWidth());
        float totalDb = 0.0f;
        for (const auto& nt : notches)
        {
            if (! nt.active) continue;
            anyActive = true;
            // analog RBJ bell: |H|^2 = ((1-w^2)^2 + (wA/Q)^2) / ((1-w^2)^2 + (w/(AQ))^2)
            const float A = std::pow (10.0f, -nt.depthDb / 40.0f);
            const float w = f / nt.freq, a = 1.0f - w * w;
            const float num = a * a + juce::square (w * A / nt.q);
            const float den = a * a + juce::square (w / (A * nt.q));
            totalDb += 10.0f * std::log10 (num / den);
        }
        const float y = yForDb (totalDb * scale);
        if (px == 0) response.startNewSubPath ((float) px, y);
        else         response.lineTo ((float) px, y);
    }

    if (anyActive)
    {
        g.setColour (Colours::notch);
        g.strokePath (response, juce::PathStrokeType (1.8f));

        for (const auto& nt : notches)
        {
            if (! nt.active) continue;
            const float x = xForFreq (nt.freq);
            const float y = yForDb (-nt.depthDb);
            g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
            g.drawText (nt.freq >= 1000.0f ? juce::String (nt.freq / 1000.0f, 2) + "k" : juce::String (juce::roundToInt (nt.freq)),
                        (int) x - 30, (int) y + 5, 60, 12, juce::Justification::centred);
        }
    }
}

//==============================================================================
DeFeedbackEditor::DeFeedbackEditor (DeFeedbackProcessor& p)
    : AudioProcessorEditor (&p), processor (p), spectrum (p.getEngine())
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (spectrum);

    addKnob (sensitivity, ParamIDs::sensitivity, "SENSITIVITY", feedbackRow);
    addKnob (speed,       ParamIDs::speed,       "SPEED",       feedbackRow);
    addKnob (release,     ParamIDs::release,     "RELEASE",     feedbackRow);
    addKnob (maxDepth,    ParamIDs::maxDepth,    "MAX DEPTH",   feedbackRow);
    addKnob (width,       ParamIDs::width,       "NOTCH Q",     feedbackRow);
    addKnob (filters,     ParamIDs::filters,     "FILTERS",     feedbackRow);
    addKnob (lowHz,       ParamIDs::lowHz,       "LOW LIMIT",   feedbackRow);
    addKnob (highHz,      ParamIDs::highHz,      "HIGH LIMIT",  feedbackRow);

    addKnob (deNoise,     ParamIDs::deNoise,     "DE-NOISE",    cleanupRow);
    addKnob (deEss,       ParamIDs::deEss,       "DE-ESS",      cleanupRow);
    addKnob (deEssFreq,   ParamIDs::deEssFreq,   "ESS FREQ",    cleanupRow);

    addKnob (mix,         ParamIDs::mix,         "MIX",         outputRow);
    addKnob (output,      ParamIDs::output,      "OUTPUT",      outputRow);

    for (auto* b : { &lockButton, &listenButton })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    lockButton.setTooltip ("Keep every deployed notch in place (no release)");
    listenButton.setTooltip ("Hear only what is being removed");
    resetButton.setTooltip ("Clear all notches");
    lockAttachment   = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::lock, lockButton);
    listenAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::listen, listenButton);

    resetButton.onClick = [this] { processor.requestReset(); };
    addAndMakeVisible (resetButton);

    status.setJustificationType (juce::Justification::centredRight);
    status.setColour (juce::Label::textColourId, Colours::text);
    addAndMakeVisible (status);

    cleanupStatus.setJustificationType (juce::Justification::centredRight);
    cleanupStatus.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (cleanupStatus);

    setResizable (true, true);
    setResizeLimits (760, 560, 1700, 1100);
    setSize (900, 640);
    startTimerHz (10);
}

DeFeedbackEditor::~DeFeedbackEditor()
{
    setLookAndFeel (nullptr);
}

void DeFeedbackEditor::addKnob (Knob& k, const char* paramId, const juce::String& name, std::vector<Knob*>& row)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, paramId, k.slider);
    k.label.setText (name, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.label);
    row.push_back (&k);
}

void DeFeedbackEditor::timerCallback()
{
    const int n = processor.getEngine().getNumActiveNotches();
    status.setText (n == 0 ? "No feedback detected" : juce::String (n) + (n == 1 ? " notch active" : " notches active"),
                    juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, n == 0 ? Colours::dim : Colours::notch);

    cleanupStatus.setText ("de-noise -" + juce::String (processor.getDenoiser().getReductionDb(), 1) + " dB   "
                           + "de-ess -" + juce::String (processor.getDeEsser().getReductionDb(), 1) + " dB",
                           juce::dontSendNotification);
}

void DeFeedbackEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("NoFeed", 16, 10, 200, 28, juce::Justification::left);
    g.setColour (Colours::dim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Feedback suppressor  /  de-noise  /  de-ess", 112, 16, 320, 18, juce::Justification::left);

    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    for (auto [r, name] : { std::pair { feedbackHeader, "FEEDBACK" }, std::pair { cleanupHeader, "CLEAN-UP" }, std::pair { outputHeader, "OUTPUT" } })
    {
        g.setColour (Colours::accent);
        g.drawText (name, r, juce::Justification::left);
        g.setColour (Colours::grid);
        g.drawHorizontalLine (r.getBottom() - 1, (float) r.getX(), (float) r.getRight());
    }
}

void DeFeedbackEditor::layoutRow (juce::Rectangle<int> area, std::vector<Knob*>& row, int knobWidth)
{
    for (auto* k : row)
    {
        auto cell = area.removeFromLeft (knobWidth);
        k->label.setBounds (cell.removeFromTop (16));
        k->slider.setBounds (cell);
    }
}

void DeFeedbackEditor::resized()
{
    auto area = getLocalBounds().reduced (14);
    auto header = area.removeFromTop (34);

    status.setBounds (header.removeFromRight (180));
    header.removeFromRight (8);
    resetButton.setBounds (header.removeFromRight (70).reduced (0, 4));
    header.removeFromRight (6);
    listenButton.setBounds (header.removeFromRight (70).reduced (0, 4));
    header.removeFromRight (6);
    lockButton.setBounds (header.removeFromRight (70).reduced (0, 4));

    const int rowHeight = juce::jlimit (105, 140, area.getHeight() / 5);
    auto bottomRow = area.removeFromBottom (rowHeight + 20);
    area.removeFromBottom (6);
    auto topRow = area.removeFromBottom (rowHeight + 20);
    area.removeFromBottom (10);
    spectrum.setBounds (area);

    const int kw = topRow.getWidth() / (int) feedbackRow.size();

    feedbackHeader = topRow.removeFromTop (18);
    layoutRow (topRow, feedbackRow, kw);

    auto cleanupArea = bottomRow.removeFromLeft (kw * (int) cleanupRow.size());
    cleanupHeader = cleanupArea.removeFromTop (18);
    layoutRow (cleanupArea, cleanupRow, kw);

    auto outputArea = bottomRow.removeFromRight (kw * (int) outputRow.size());
    outputHeader = outputArea.removeFromTop (18);
    layoutRow (outputArea, outputRow, kw);

    cleanupStatus.setBounds (bottomRow.reduced (8, 0).withTrimmedTop (18).removeFromTop (20));
    cleanupHeader = cleanupHeader.withRight (cleanupHeader.getRight() - 8);
    outputHeader  = outputHeader.withX (outputHeader.getX() + 8);
}
