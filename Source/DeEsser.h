#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace dfb
{

/**
    Zero-latency dynamic de-esser.

    Detection: the level above the split frequency (state-variable high-pass) is compared to
    the full-band level. Sibilants ("s", "sh", "t") put most of their energy up there while
    vowels don't, so this relative measure catches esses regardless of how loud the talker is.

    Gain stage: a dynamic high-shelf at the split frequency that dips only while an ess is
    present. At 0 dB of reduction the shelf is mathematically an identity, so the de-esser is
    bit-transparent between sibilants and when switched off.
*/
class DeEsser
{
public:
    static constexpr int kMaxChannels = 8;

    void prepare (double sampleRate);
    void reset();

    /** amount 0..1 (0 = off), frequency = split / detection frequency in Hz. */
    void process (float* const* channels, int numChannels, int numSamples, float amount, float frequencyHz);

    /** Current gain reduction applied to the sibilance band, in dB (positive). */
    float getReductionDb() const noexcept { return reductionDb.load(); }

private:
    double sampleRate = 48000.0;
    std::array<float, kMaxChannels> ic1 {}, ic2 {};   // detector high-pass
    std::array<float, kMaxChannels> sc1 {}, sc2 {};   // high-shelf gain stage
    float gBase = 0.0f, s1 = 1.0f, s2 = 0.0f, s3 = 0.0f, m0 = 1.0f, m1 = 0.0f, m2 = 0.0f;
    float hpEnv = 0.0f, fullEnv = 0.0f, grDb = 0.0f;
    float lastFreq = -1.0f, g = 0.0f, k = 1.414f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    std::atomic<float> reductionDb { 0.0f };
};

} // namespace dfb
