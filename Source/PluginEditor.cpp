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
    const juce::Colour input      { 0xff5d6673 };
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
SpectrumView::SpectrumView (const DeFeedbackProcessor& p) : processor (p)
{
    inDb.fill (-120.0f);
    outDb.fill (-120.0f);
    startTimerHz (30);
}

void SpectrumView::timerCallback()
{
    processor.getAi().getDisplay (inDb.data(), outDb.data(), (int) inDb.size());
    showNotches = processor.isNotchAssistOn();
    notches = processor.getEngine().getNotches();
    repaint();
}

float SpectrumView::xForFreq (float f) const
{
    return (float) getWidth() * std::log (f / 20.0f) / std::log (1000.0f);
}

float SpectrumView::yForDb (float db) const
{
    constexpr float top = 0.0f, bottom = -96.0f;
    return juce::jmap (juce::jlimit (bottom, top, db), top, bottom, 0.0f, (float) getHeight());
}

juce::Path SpectrumView::makePath (const float* db, int n, bool closed) const
{
    juce::Path p;
    const float w = (float) getWidth(), h = (float) getHeight();
    for (int i = 0; i < n; ++i)
    {
        const float x = w * (float) i / (float) (n - 1), y = yForDb (db[i]);
        if (i == 0) { if (closed) { p.startNewSubPath (x, h); p.lineTo (x, y); } else p.startNewSubPath (x, y); }
        else p.lineTo (x, y);
    }
    if (closed) { p.lineTo (w, h); p.closeSubPath(); }
    return p;
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (area, 6.0f);

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

    const int n = (int) inDb.size();

    // what goes in (grey) vs. what the AI keeps (cyan): the gap between them is removed
    g.setColour (Colours::input.withAlpha (0.35f));
    g.fillPath (makePath (inDb.data(), n, true));
    g.setColour (Colours::input);
    g.strokePath (makePath (inDb.data(), n, false), juce::PathStrokeType (1.2f));

    g.setGradientFill (juce::ColourGradient (Colours::accent.withAlpha (0.45f), 0.0f, 0.0f,
                                             Colours::accent.withAlpha (0.05f), 0.0f, area.getHeight(), false));
    g.fillPath (makePath (outDb.data(), n, true));
    g.setColour (Colours::accent);
    g.strokePath (makePath (outDb.data(), n, false), juce::PathStrokeType (1.6f));

    // legend
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (Colours::input.brighter (0.4f));
    g.drawText ("INPUT", getWidth() - 140, 6, 60, 14, juce::Justification::right);
    g.setColour (Colours::accent);
    g.drawText ("VOICE OUT", getWidth() - 76, 6, 70, 14, juce::Justification::right);

    if (! showNotches)
        return;

    juce::Path response;
    bool anyActive = false;
    for (int px = 0; px <= getWidth(); px += 2)
    {
        const float f = 20.0f * std::pow (1000.0f, (float) px / (float) getWidth());
        float totalDb = 0.0f;
        for (const auto& nt : notches)
        {
            if (! nt.active) continue;
            anyActive = true;
            const float A = std::pow (10.0f, -nt.depthDb / 40.0f);
            const float w = f / nt.freq, a = 1.0f - w * w;
            totalDb += 10.0f * std::log10 ((a * a + juce::square (w * A / nt.q)) / (a * a + juce::square (w / (A * nt.q))));
        }
        const float y = yForDb (totalDb);
        if (px == 0) response.startNewSubPath ((float) px, y);
        else         response.lineTo ((float) px, y);
    }
    if (anyActive)
    {
        g.setColour (Colours::notch);
        g.strokePath (response, juce::PathStrokeType (1.6f));
    }
}

//==============================================================================
DeFeedbackEditor::DeFeedbackEditor (DeFeedbackProcessor& p)
    : AudioProcessorEditor (&p), processor (p), spectrum (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (spectrum);

    addKnob (amount,      ParamIDs::aiAmount,    "AMOUNT",      mainRow);
    addKnob (deEss,       ParamIDs::deEss,       "DE-ESS",      mainRow);
    addKnob (deEssFreq,   ParamIDs::deEssFreq,   "ESS FREQ",    mainRow);
    addKnob (output,      ParamIDs::output,      "OUTPUT",      mainRow);

    addKnob (sensitivity, ParamIDs::sensitivity, "SENSITIVITY", notchRow);
    addKnob (speed,       ParamIDs::speed,       "SPEED",       notchRow);
    addKnob (release,     ParamIDs::release,     "RELEASE",     notchRow);
    addKnob (maxDepth,    ParamIDs::maxDepth,    "MAX DEPTH",   notchRow);
    addKnob (width,       ParamIDs::width,       "NOTCH Q",     notchRow);
    addKnob (filters,     ParamIDs::filters,     "FILTERS",     notchRow);
    addKnob (lowHz,       ParamIDs::lowHz,       "LOW LIMIT",   notchRow);
    addKnob (highHz,      ParamIDs::highHz,      "HIGH LIMIT",  notchRow);

    amount.label.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    amount.label.setColour (juce::Label::textColourId, Colours::accent);
    amount.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 20);

    for (auto* b : { &listenButton, &notchButton, &lockButton })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    listenButton.setTooltip ("Hear only what the AI is removing");
    notchButton.setTooltip ("Add classic adaptive notch filters after the AI (optional)");
    lockButton.setTooltip ("Keep Notch Assist's notches in place");
    resetButton.setTooltip ("Clear Notch Assist's notches");
    listenAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::listen, listenButton);
    notchAttachment  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::notchAssist, notchButton);
    lockAttachment   = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamIDs::lock, lockButton);
    resetButton.onClick = [this] { processor.requestReset(); };
    addAndMakeVisible (resetButton);

    removedLabel.setJustificationType (juce::Justification::centred);
    removedLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addAndMakeVisible (removedLabel);

    status.setJustificationType (juce::Justification::centredRight);
    status.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (status);

    setResizable (true, true);
    setResizeLimits (760, 600, 1700, 1100);
    setSize (900, 660);
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
    const float removed = processor.getAi().getReductionDb();
    removedLabel.setText (juce::String (removed, 1) + " dB removed", juce::dontSendNotification);
    removedLabel.setColour (juce::Label::textColourId, removed > 3.0f ? Colours::notch : Colours::dim);

    const bool notchOn = processor.isNotchAssistOn();
    for (auto* k : notchRow)
    {
        k->slider.setAlpha (notchOn ? 1.0f : 0.35f);
        k->label.setAlpha (notchOn ? 1.0f : 0.35f);
    }
    lockButton.setEnabled (notchOn);
    resetButton.setEnabled (notchOn);

    const int notches = notchOn ? processor.getEngine().getNumActiveNotches() : 0;
    status.setText ("AI active" + (notchOn ? "  |  " + juce::String (notches) + " notches" : juce::String())
                    + "  |  de-ess -" + juce::String (processor.getDeEsser().getReductionDb(), 1) + " dB",
                    juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, Colours::dim);
    repaint (notchHeader);
}

void DeFeedbackEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("NoFeed", 16, 10, 200, 28, juce::Justification::left);
    g.setColour (Colours::accent);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("AI", 106, 17, 20, 16, juce::Justification::left);
    g.setColour (Colours::dim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("feedback suppressor  /  voice isolation", 126, 16, 300, 18, juce::Justification::left);

    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (Colours::accent);
    g.drawText ("AI VOICE ISOLATION", mainHeader, juce::Justification::left);
    g.setColour (Colours::grid);
    g.drawHorizontalLine (mainHeader.getBottom() - 1, (float) mainHeader.getX(), (float) mainHeader.getRight());

    g.setColour (processor.isNotchAssistOn() ? Colours::accent : Colours::dim);
    g.drawText ("NOTCH ASSIST (OPTIONAL)", notchHeader, juce::Justification::left);
    g.setColour (Colours::grid);
    g.drawHorizontalLine (notchHeader.getBottom() - 1, (float) notchHeader.getX(), (float) notchHeader.getRight());

    // ring around the main knob
    g.setColour (Colours::accent.withAlpha (0.08f));
    g.fillRoundedRectangle (amountArea.toFloat(), 8.0f);
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

    status.setBounds (header.removeFromRight (250));
    header.removeFromRight (8);
    resetButton.setBounds (header.removeFromRight (64).reduced (0, 4));
    header.removeFromRight (6);
    lockButton.setBounds (header.removeFromRight (56).reduced (0, 4));
    header.removeFromRight (6);
    notchButton.setBounds (header.removeFromRight (110).reduced (0, 4));
    header.removeFromRight (6);
    listenButton.setBounds (header.removeFromRight (70).reduced (0, 4));

    auto notchArea = area.removeFromBottom (juce::jlimit (100, 130, area.getHeight() / 6) + 18);
    area.removeFromBottom (8);
    auto mainArea = area.removeFromBottom (juce::jlimit (150, 190, area.getHeight() / 3) + 18);
    area.removeFromBottom (10);
    spectrum.setBounds (area);

    mainHeader = mainArea.removeFromTop (18);
    mainArea.removeFromTop (4);
    amountArea = mainArea.removeFromLeft (260);
    {
        auto a = amountArea.reduced (6);
        removedLabel.setBounds (a.removeFromBottom (18));
        amount.label.setBounds (a.removeFromTop (20));
        amount.slider.setBounds (a);
    }
    mainArea.removeFromLeft (24);
    std::vector<Knob*> rest { &deEss, &deEssFreq, &output };
    const int cellH = juce::jmin (120, mainArea.getHeight());
    layoutRow (mainArea.withSizeKeepingCentre (mainArea.getWidth(), cellH).withX (mainArea.getX()),
               rest, juce::jmin (110, mainArea.getWidth() / 3));

    notchHeader = notchArea.removeFromTop (18);
    layoutRow (notchArea, notchRow, notchArea.getWidth() / (int) notchRow.size());
}
