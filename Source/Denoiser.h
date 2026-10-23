#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace dfb
{

/**
    Adaptive spectral noise reduction (STFT, 75 % overlap, sqrt-Hann analysis/synthesis).

    The noise floor of every frequency bin is tracked continuously with minimum statistics
    (it falls immediately to the quietest level seen and creeps up slowly), so the de-noiser
    learns hiss, hum, room tone and fan noise on its own during pauses — no "learn" button.
    A Wiener-style gain with over-subtraction, a gain floor and time/frequency smoothing keeps
    "musical noise" artefacts low.

    Latency is one FFT frame (~10.7 ms at 44.1/48 kHz) and is constant, so hosts can compensate.
*/
class Denoiser
{
public:
    static constexpr int kMaxChannels = 8;

    void prepare (double sampleRate, int numChannels);
    void reset();

    /** amount 0..1 (0 = transparent, only delays the signal). */
    void process (float* const* channels, int numChannels, int numSamples, float amount);

    int getLatencySamples() const noexcept    { return fftSize; }
    float getReductionDb() const noexcept     { return reductionDb.load(); }

private:
    struct Channel
    {
        std::vector<float> inRing, outAccum, noise, smoothPower, gain;
        bool primed = false;
    };

    void processFrame (Channel&, float amount);

    double sampleRate = 48000.0;
    int fftOrder = 9, fftSize = 512, hopSize = 128, numBins = 257;
    int pos = 0, hopCounter = 0, numChannelsPrepared = 0;
    float riseFactor = 1.0f;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, frame, smoothedGain;
    std::array<Channel, kMaxChannels> chans;
    std::atomic<float> reductionDb { 0.0f };
};

} // namespace dfb
