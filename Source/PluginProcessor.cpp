#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::NormalisableRange<float> hzRange (float lo, float hi)
    {
        juce::NormalisableRange<float> r (lo, hi, 1.0f);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    juce::String percentText (float v, int)  { return juce::String (juce::roundToInt (v)) + " %"; }
    juce::String hzText (float v, int)       { return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz"; }
}

DeFeedbackProcessor::DeFeedbackProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "NoFeed", createLayout())
{
    pSensitivity = apvts.getRawParameterValue (ParamIDs::sensitivity);
    pSpeed       = apvts.getRawParameterValue (ParamIDs::speed);
    pRelease     = apvts.getRawParameterValue (ParamIDs::release);
    pMaxDepth    = apvts.getRawParameterValue (ParamIDs::maxDepth);
    pWidth       = apvts.getRawParameterValue (ParamIDs::width);
    pFilters     = apvts.getRawParameterValue (ParamIDs::filters);
    pLowHz       = apvts.getRawParameterValue (ParamIDs::lowHz);
    pHighHz      = apvts.getRawParameterValue (ParamIDs::highHz);
    pMix         = apvts.getRawParameterValue (ParamIDs::mix);
    pOutput      = apvts.getRawParameterValue (ParamIDs::output);
    pLock        = apvts.getRawParameterValue (ParamIDs::lock);
    pListen      = apvts.getRawParameterValue (ParamIDs::listen);
    pDeNoise     = apvts.getRawParameterValue (ParamIDs::deNoise);
    pDeEss       = apvts.getRawParameterValue (ParamIDs::deEss);
    pDeEssFreq   = apvts.getRawParameterValue (ParamIDs::deEssFreq);
}

juce::AudioProcessorValueTreeState::ParameterLayout DeFeedbackProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto pct = [] (const char* id, const char* name, float def)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 100.0f, 0.1f), def,
                                                      AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (percentText));
    };

    layout.add (pct (ParamIDs::sensitivity, "Sensitivity", 50.0f));
    layout.add (pct (ParamIDs::speed, "Speed", 60.0f));

    NormalisableRange<float> releaseRange (0.5f, 30.0f, 0.01f);
    releaseRange.setSkewForCentre (5.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::release, 1 }, "Release", releaseRange, 5.0f,
                                                       AudioParameterFloatAttributes().withLabel ("s")
                                                           .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " s"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::maxDepth, 1 }, "Max Depth", NormalisableRange<float> (6.0f, 48.0f, 0.1f), 30.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")
                                                           .withStringFromValueFunction ([] (float v, int) { return "-" + String (v, 1) + " dB"; })));

    NormalisableRange<float> qRange (4.0f, 100.0f, 0.1f);
    qRange.setSkewForCentre (25.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::width, 1 }, "Notch Q", qRange, 30.0f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return "Q " + String (v, 1); })));

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamIDs::filters, 1 }, "Filters", 1, dfb::FeedbackEngine::kMaxNotches, 12));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::lowHz, 1 }, "Low Limit", hzRange (40.0f, 2000.0f), 80.0f,
                                                       AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::highHz, 1 }, "High Limit", hzRange (2000.0f, 20000.0f), 16000.0f,
                                                       AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));

    layout.add (pct (ParamIDs::deNoise, "De-Noise", 0.0f));
    layout.add (pct (ParamIDs::deEss, "De-Ess", 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::deEssFreq, 1 }, "De-Ess Freq", hzRange (3000.0f, 12000.0f), 6000.0f,
                                                       AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));

    layout.add (pct (ParamIDs::mix, "Mix", 100.0f));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::output, 1 }, "Output", NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")
                                                           .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::lock, 1 }, "Lock Filters", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::listen, 1 }, "Listen Removed", false));

    return layout;
}

bool DeFeedbackProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    // mono -> mono, stereo -> stereo, and mono -> stereo (e.g. SuperRack mono-to-stereo racks)
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

void DeFeedbackProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int numCh = juce::jmax (1, getTotalNumOutputChannels());
    engine.prepare (sampleRate, numCh);
    denoiser.prepare (sampleRate, numCh);
    deEsser.prepare (sampleRate);
    setLatencySamples (denoiser.getLatencySamples());

    dryBuffer.setSize (numCh, juce::jmax (1, samplesPerBlock));
    dryDelay.setSize (numCh, denoiser.getLatencySamples());
    dryDelay.clear();
    dryDelayPos = 0;

    mixSmoothed.reset (sampleRate, 0.05);
    outputSmoothed.reset (sampleRate, 0.05);
    mixSmoothed.setCurrentAndTargetValue (pMix->load() * 0.01f);
    outputSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pOutput->load()));
}

void DeFeedbackProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    // mono in -> stereo out: feed the input to every output channel
    for (int ch = numIn; ch < numOut; ++ch)
    {
        if (numIn > 0) buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);
        else           buffer.clear (ch, 0, numSamples);
    }

    // some hosts' plugin scanners call process before prepareToPlay: pass audio through untouched
    if (dryDelay.getNumSamples() == 0 || numSamples <= 0)
        return;

    if (resetRequested.exchange (false))
        engine.reset();   // clears the notches only; de-noise keeps its learned noise floor

    dfb::EngineParams p;
    p.sensitivity    = pSensitivity->load() * 0.01f;
    p.speed          = pSpeed->load() * 0.01f;
    p.releaseSeconds = pRelease->load();
    p.maxDepthDb     = pMaxDepth->load();
    p.q              = pWidth->load();
    p.maxNotches     = (int) pFilters->load();
    p.minHz          = pLowHz->load();
    p.maxHz          = pHighHz->load();
    p.lock           = pLock->load() > 0.5f;

    const bool listen = pListen->load() > 0.5f;
    mixSmoothed.setTargetValue (pMix->load() * 0.01f);
    outputSmoothed.setTargetValue (juce::Decibels::decibelsToGain (pOutput->load()));

    // keep the dry signal for Mix / Listen
    if (dryBuffer.getNumSamples() < numSamples || dryBuffer.getNumChannels() < numOut)
        dryBuffer.setSize (juce::jmax (1, numOut), numSamples, false, false, true);
    for (int ch = 0; ch < numOut; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    // chain: feedback suppression -> noise reduction -> de-essing
    auto* const* channels = buffer.getArrayOfWritePointers();
    engine.process (channels, numOut, numSamples, p);
    denoiser.process (channels, numOut, numSamples, pDeNoise->load() * 0.01f);
    deEsser.process (channels, numOut, numSamples, pDeEss->load() * 0.01f, pDeEssFreq->load());

    const int delayLen = dryDelay.getNumSamples();
    for (int i = 0; i < numSamples; ++i)
    {
        const float mix  = mixSmoothed.getNextValue();
        const float gain = outputSmoothed.getNextValue();

        for (int ch = 0; ch < numOut; ++ch)
        {
            const float dry = dryDelay.getSample (ch, dryDelayPos);
            dryDelay.setSample (ch, dryDelayPos, dryBuffer.getSample (ch, i));

            const float wet = buffer.getSample (ch, i);
            const float out = listen ? (dry - wet) : (dry + mix * (wet - dry));
            buffer.setSample (ch, i, out * gain);
        }

        if (++dryDelayPos >= delayLen)
            dryDelayPos = 0;
    }
}

juce::AudioProcessorEditor* DeFeedbackProcessor::createEditor()
{
    return new DeFeedbackEditor (*this);
}

void DeFeedbackProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DeFeedbackProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DeFeedbackProcessor();
}
