#include "DeEsser.h"

#include <cmath>

namespace dfb
{

void DeEsser::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    lastFreq = -1.0f;
    reset();
}

void DeEsser::reset()
{
    ic1.fill (0.0f);
    ic2.fill (0.0f);
    sc1.fill (0.0f);
    sc2.fill (0.0f);
    hpEnv = fullEnv = grDb = 0.0f;
    reductionDb.store (0.0f);
}

void DeEsser::process (float* const* channels, int numChannels, int numSamples, float amount, float frequencyHz)
{
    numChannels = juce::jmin (numChannels, kMaxChannels);
    amount = juce::jlimit (0.0f, 1.0f, amount);

    const float freq = juce::jlimit (1000.0f, (float) sampleRate * 0.45f, frequencyHz);
    if (std::abs (freq - lastFreq) > 0.01f)
    {
        lastFreq = freq;
        g  = std::tan (juce::MathConstants<float>::pi * freq / (float) sampleRate);
        k  = 1.41421356f;   // Butterworth
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        gBase = g;
    }

    // detector: fast attack catches the start of an "s", release long enough not to flutter
    const auto sr = (float) sampleRate;
    const float envAtk = 1.0f - std::exp (-1.0f / (0.0005f * sr));
    const float envRel = 1.0f - std::exp (-1.0f / (0.030f * sr));
    const float grAtk  = 1.0f - std::exp (-1.0f / (0.0008f * sr));
    const float grRel  = 1.0f - std::exp (-1.0f / (0.060f * sr));

    const float maxReduction = 20.0f * amount;
    const float relThreshold = juce::jmap (amount, -3.0f, -14.0f);   // band level vs. full level
    const float gateDb = -60.0f;

    float maxGr = 0.0f;
    constexpr int kControlRate = 8;

    for (int i = 0; i < numSamples; ++i)
    {
        // detector: high-passed level vs. full-band level, linked across channels
        float sHp = 0.0f, sFull = 0.0f;

        for (int c = 0; c < numChannels; ++c)
        {
            const float v0 = channels[c][i];
            const float v3 = v0 - ic2[(size_t) c];
            const float v1 = a1 * ic1[(size_t) c] + a2 * v3;
            const float v2 = ic2[(size_t) c] + a2 * ic1[(size_t) c] + a3 * v3;
            ic1[(size_t) c] = 2.0f * v1 - ic1[(size_t) c];
            ic2[(size_t) c] = 2.0f * v2 - ic2[(size_t) c];

            const float hp = v0 - k * v1 - v2;
            sHp   = juce::jmax (sHp, hp * hp);
            sFull = juce::jmax (sFull, v0 * v0);
        }

        hpEnv   += (sHp   - hpEnv)   * (sHp   > hpEnv   ? envAtk : envRel);
        fullEnv += (sFull - fullEnv) * (sFull > fullEnv ? envAtk : envRel);

        float target = 0.0f;
        if (maxReduction > 0.0f)
        {
            const float hpDb   = 10.0f * std::log10 (hpEnv + 1.0e-20f);
            const float fullDb = 10.0f * std::log10 (fullEnv + 1.0e-20f);
            const float excess = (hpDb - fullDb) - relThreshold;
            if (hpDb > gateDb && excess > 0.0f)
                target = juce::jmin (maxReduction, excess * 1.5f);
        }

        grDb += (target - grDb) * (target > grDb ? grAtk : grRel);
        maxGr = juce::jmax (maxGr, grDb);

        // gain stage: dynamic high-shelf (Simper SVF); identity when grDb == 0
        if (i % kControlRate == 0)
        {
            const float A = std::pow (10.0f, -grDb / 40.0f);
            const float gs = gBase * std::sqrt (A);
            const float ks = 1.41421356f;
            s1 = 1.0f / (1.0f + gs * (gs + ks));
            s2 = gs * s1;
            s3 = gs * s2;
            m0 = A * A;
            m1 = ks * (1.0f - A) * A;
            m2 = 1.0f - A * A;
        }

        for (int c = 0; c < numChannels; ++c)
        {
            const float v0 = channels[c][i];
            const float v3 = v0 - sc2[(size_t) c];
            const float v1 = s1 * sc1[(size_t) c] + s2 * v3;
            const float v2 = sc2[(size_t) c] + s2 * sc1[(size_t) c] + s3 * v3;
            sc1[(size_t) c] = 2.0f * v1 - sc1[(size_t) c];
            sc2[(size_t) c] = 2.0f * v2 - sc2[(size_t) c];
            channels[c][i] = m0 * v0 + m1 * v1 + m2 * v2;
        }
    }

    reductionDb.store (maxGr);
}

} // namespace dfb
