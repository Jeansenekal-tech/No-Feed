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
    The neural network: log-power spectrum in, per-bin voice mask out.

    Linear(513 -> H) + ReLU -> GRU(H) -> GRU(H) -> Linear([gru1, gru2] -> 513) + sigmoid.
    Weights come from the PyTorch training script (see `training/`), packed by export.py.
    Matrices are stored column-major so every mat-vec is a sequence of `y += column * x[j]`
    updates, which compilers vectorise without needing fast-math.
*/
class VoiceMaskModel
{
public:
    static constexpr int kBins = 513;

    bool load (const void* data, size_t size);
    bool isLoaded() const noexcept { return hidden > 0; }
    int getHiddenSize() const noexcept { return hidden; }

    void reset();
    void forward (const float* logPower, float* mask);

private:
    struct Dense { std::vector<float> wT, b; int in = 0, out = 0; };   // wT is [in][out]
    struct Gru   { std::vector<float> wihT, whhT, bih, bhh; int in = 0, hid = 0; };

    static void matVecAdd (const std::vector<float>& wT, int in, int out, const float* x, float* y);
    void gruStep (const Gru& g, const float* x, std::vector<float>& h);

    int hidden = 0;
    std::vector<float> mean, invStd;
    Dense inp, out;
    Gru gru1, gru2;
    std::vector<float> x0, h1, h2, cat, gi, gh;
};

/**
    Real-time AI voice isolation: removes feedback, the loop's re-emission of the voice,
    room reverb and noise, keeping the dry voice. This is the approach of tools like
    Alpha Labs De-Feedback: the network decides what is voice, everything else is removed
    before it can circulate and build up into a howl.

    Low-latency asymmetric STFT: 1024-sample analysis window (fine frequency resolution) with a
    256-sample synthesis window and 128-sample hop, so the algorithmic latency is only 256 samples
    (5.3 ms at 48 kHz). Above 48 kHz everything scales by a power of two so the network always
    sees the same frequency resolution and frame rate.
*/
class AiVoiceEngine
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kDisplayPoints = 256;

    AiVoiceEngine();

    bool loadModel (const void* data, size_t size)   { return model.load (data, size); }
    bool hasModel() const noexcept                    { return model.isLoaded(); }

    void prepare (double sampleRate, int numChannels);
    void reset();

    /** amount 0..1: how much of the non-voice signal is removed. */
    void process (float* const* channels, int numChannels, int numSamples, float amount);

    int getLatencySamples() const noexcept           { return 2 * synthHalf; }
    float getReductionDb() const noexcept            { return reductionDb.load(); }
    void getDisplay (float* inDb, float* outDb, int numPoints) const;

private:
    void processFrame (float amount);
    void publishDisplay();

    VoiceMaskModel model;
    double sampleRate = 48000.0;
    int scale = 1, fftSize = 1024, hop = 128, synthHalf = 128, numBins = 513;
    int numChannelsPrepared = 1, pos = 0, hopCounter = 0;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> anaWin, synWin, frame, monoSpec, logPow, mask, gains;
    std::array<std::vector<float>, kMaxChannels> inRing, outAccum, chanSpec;

    std::array<float, kDisplayPoints> dispInLocal {}, dispOutLocal {};
    std::array<std::atomic<float>, kDisplayPoints> dispIn, dispOut;
    std::atomic<float> reductionDb { 0.0f };
};

} // namespace dfb
