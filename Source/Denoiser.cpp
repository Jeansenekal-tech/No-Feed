#include "Denoiser.h"

#include <cmath>

namespace dfb
{

void Denoiser::prepare (double newSampleRate, int numChannels)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    numChannelsPrepared = juce::jlimit (1, kMaxChannels, numChannels);

    // ~10 ms frames at any sample rate
    fftOrder = sampleRate <= 50000.0 ? 9 : (sampleRate <= 100000.0 ? 10 : 11);
    fftSize  = 1 << fftOrder;
    hopSize  = fftSize / 4;
    numBins  = fftSize / 2 + 1;

    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    // periodic sqrt-Hann: analysis * synthesis = Hann, which sums to 2 at 75 % overlap
    window.resize ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize));

    frame.assign ((size_t) fftSize * 2, 0.0f);
    smoothedGain.assign ((size_t) numBins, 1.0f);

    // noise floor may rise at most ~3 dB per second (minimum-statistics tracking)
    const float frameSec = (float) hopSize / (float) sampleRate;
    riseFactor = std::pow (10.0f, 3.0f * frameSec / 10.0f);

    for (auto& c : chans)
    {
        c.inRing.assign ((size_t) fftSize, 0.0f);
        c.outAccum.assign ((size_t) fftSize, 0.0f);
        c.noise.assign ((size_t) numBins, 0.0f);
        c.smoothPower.assign ((size_t) numBins, 0.0f);
        c.gain.assign ((size_t) numBins, 1.0f);
    }

    reset();
}

void Denoiser::reset()
{
    pos = 0;
    hopCounter = 0;
    for (auto& c : chans)
    {
        std::fill (c.inRing.begin(), c.inRing.end(), 0.0f);
        std::fill (c.outAccum.begin(), c.outAccum.end(), 0.0f);
        std::fill (c.gain.begin(), c.gain.end(), 1.0f);
        c.primed = false;
    }
    reductionDb.store (0.0f);
}

void Denoiser::process (float* const* channels, int numChannels, int numSamples, float amount)
{
    if (fft == nullptr)
        return;

    numChannels = juce::jmin (numChannels, numChannelsPrepared);
    amount = juce::jlimit (0.0f, 1.0f, amount);
    const int mask = fftSize - 1;

    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            auto& ch = chans[(size_t) c];
            const float x = channels[c][i];
            channels[c][i] = ch.outAccum[(size_t) pos];
            ch.outAccum[(size_t) pos] = 0.0f;
            ch.inRing[(size_t) pos] = x;
        }

        pos = (pos + 1) & mask;

        if (++hopCounter >= hopSize)
        {
            hopCounter = 0;
            for (int c = 0; c < numChannels; ++c)
                processFrame (chans[(size_t) c], amount);
        }
    }
}

void Denoiser::processFrame (Channel& ch, float amount)
{
    const int mask = fftSize - 1;

    // `pos` now points at the oldest sample of the last fftSize inputs
    for (int j = 0; j < fftSize; ++j)
        frame[(size_t) j] = ch.inRing[(size_t) ((pos + j) & mask)] * window[(size_t) j];
    std::fill (frame.begin() + fftSize, frame.end(), 0.0f);

    fft->performRealOnlyForwardTransform (frame.data(), true);

    const float floorGain  = juce::Decibels::decibelsToGain (-24.0f * amount);
    const float overSub    = 1.0f + 2.0f * amount;
    constexpr float bias   = 2.0f;    // minimum of a fluctuating estimate sits below its mean

    double inPow = 0.0, outPow = 0.0;

    for (int k = 0; k < numBins; ++k)
    {
        const float re = frame[(size_t) (2 * k)], im = frame[(size_t) (2 * k + 1)];
        const float p = re * re + im * im;

        // --- noise floor tracking -------------------------------------------------------
        auto& sp = ch.smoothPower[(size_t) k];
        auto& nz = ch.noise[(size_t) k];
        sp = ch.primed ? 0.85f * sp + 0.15f * p : p;
        if (! ch.primed || sp < nz) nz = sp;
        else                        nz *= riseFactor;

        // --- spectral gain ----------------------------------------------------------------
        float target = 1.0f;
        if (amount > 0.0f && p > 1.0e-24f)
            target = juce::jlimit (floorGain, 1.0f, 1.0f - overSub * bias * nz / p);

        auto& gk = ch.gain[(size_t) k];
        gk += (target - gk) * (target > gk ? 0.8f : 0.35f);   // open fast for speech onsets
    }
    ch.primed = true;

    // light smoothing across frequency reduces isolated "musical noise" bins
    for (int k = 0; k < numBins; ++k)
    {
        const float l = ch.gain[(size_t) juce::jmax (0, k - 1)];
        const float r = ch.gain[(size_t) juce::jmin (numBins - 1, k + 1)];
        smoothedGain[(size_t) k] = 0.25f * l + 0.5f * ch.gain[(size_t) k] + 0.25f * r;
    }

    for (int k = 0; k < numBins; ++k)
    {
        const float re = frame[(size_t) (2 * k)], im = frame[(size_t) (2 * k + 1)];
        const float p = re * re + im * im;
        const float gk = smoothedGain[(size_t) k];
        frame[(size_t) (2 * k)]     = re * gk;
        frame[(size_t) (2 * k + 1)] = im * gk;
        inPow  += p;
        outPow += p * gk * gk;
    }

    fft->performRealOnlyInverseTransform (frame.data());

    constexpr float olaScale = 0.5f;
    for (int j = 0; j < fftSize; ++j)
        ch.outAccum[(size_t) ((pos + j) & mask)] += frame[(size_t) j] * window[(size_t) j] * olaScale;

    const float red = inPow > 1.0e-12 ? (float) (10.0 * std::log10 (inPow / juce::jmax (outPow, 1.0e-30))) : 0.0f;
    reductionDb.store (red);
}

} // namespace dfb
