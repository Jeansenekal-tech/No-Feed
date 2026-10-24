#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "FeedbackEngine.h"
#include "DeEsser.h"
#include "AiVoiceEngine.h"

namespace ParamIDs
{
    inline constexpr const char* sensitivity = "sensitivity";
    inline constexpr const char* speed       = "speed";
    inline constexpr const char* release     = "release";
    inline constexpr const char* maxDepth    = "maxDepth";
    inline constexpr const char* width       = "width";
    inline constexpr const char* filters     = "filters";
    inline constexpr const char* lowHz       = "lowHz";
    inline constexpr const char* highHz      = "highHz";
    inline constexpr const char* output      = "output";
    inline constexpr const char* lock        = "lock";
    inline constexpr const char* listen      = "listen";
    inline constexpr const char* aiAmount    = "aiAmount";
    inline constexpr const char* notchAssist = "notchAssist";
    inline constexpr const char* deEss       = "deEss";
    inline constexpr const char* deEssFreq   = "deEssFreq";
}

class DeFeedbackProcessor : public juce::AudioProcessor
{
public:
    DeFeedbackProcessor();
    ~DeFeedbackProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return JucePlugin_Name; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }

    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Clears every deployed notch of Notch Assist (safe to call from the UI thread). */
    void requestReset() noexcept                           { resetRequested.store (true); }

    const dfb::FeedbackEngine& getEngine() const noexcept  { return engine; }
    const dfb::DeEsser& getDeEsser() const noexcept        { return deEsser; }
    const dfb::AiVoiceEngine& getAi() const noexcept       { return ai; }
    bool isNotchAssistOn() const noexcept                  { return pNotchAssist->load() > 0.5f; }
    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    dfb::AiVoiceEngine ai;
    dfb::FeedbackEngine engine;          // "Notch Assist"
    dfb::DeEsser deEsser;
    juce::AudioBuffer<float> dryBuffer, dryDelay;   // dry path delayed by the AI latency (for Listen)
    int dryDelayPos = 0;
    bool notchAssistWasOn = false;
    juce::SmoothedValue<float> outputSmoothed;
    std::atomic<bool> resetRequested { false };

    std::atomic<float>* pSensitivity = nullptr;
    std::atomic<float>* pSpeed       = nullptr;
    std::atomic<float>* pRelease     = nullptr;
    std::atomic<float>* pMaxDepth    = nullptr;
    std::atomic<float>* pWidth       = nullptr;
    std::atomic<float>* pFilters     = nullptr;
    std::atomic<float>* pLowHz       = nullptr;
    std::atomic<float>* pHighHz      = nullptr;
    std::atomic<float>* pOutput      = nullptr;
    std::atomic<float>* pLock        = nullptr;
    std::atomic<float>* pListen      = nullptr;
    std::atomic<float>* pAiAmount    = nullptr;
    std::atomic<float>* pNotchAssist = nullptr;
    std::atomic<float>* pDeEss       = nullptr;
    std::atomic<float>* pDeEssFreq   = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeFeedbackProcessor)
};
