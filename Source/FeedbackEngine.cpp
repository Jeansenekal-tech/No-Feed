#include "FeedbackEngine.h"

#include <algorithm>
#include <cmath>

namespace dfb
{

namespace
{
    constexpr float kTiny = 1.0e-20f;

    inline float toDb (float power) noexcept    { return 10.0f * std::log10 (power + kTiny); }
    inline float lerp (float a, float b, float t) { return a + (b - a) * t; }
}

FeedbackEngine::FeedbackEngine()
{
    for (auto& d : displayDb) d.store (-120.0f);
    for (int i = 0; i < kMaxNotches; ++i)
    {
        uiFreq[(size_t) i].store (1000.0f);
        uiDepth[(size_t) i].store (0.0f);
        uiQ[(size_t) i].store (30.0f);
        uiActive[(size_t) i].store (false);
    }
}

void FeedbackEngine::prepare (double newSampleRate, int numChannels)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    numChannelsPrepared = juce::jlimit (1, kMaxChannels, numChannels);

    // ~10 Hz resolution regardless of sample rate
    fftOrder = sampleRate <= 50000.0 ? 12 : (sampleRate <= 100000.0 ? 13 : 14);
    fftSize  = 1 << fftOrder;
    hopSize  = fftSize / 8;
    binHz    = (float) (sampleRate / fftSize);

    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    window.resize ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

    const auto numBins = (size_t) (fftSize / 2 + 1);
    ring.assign ((size_t) fftSize, 0.0f);
    fftData.assign ((size_t) fftSize * 2, 0.0f);
    power.assign (numBins, 0.0f);
    powerDb.assign (numBins, -200.0f);
    prefix.assign (numBins + 1, 0.0f);
    pnprByBin.assign (numBins, 0.0f);
    isLocalMax.assign (numBins, 0);
    peaks.reserve (numBins);

    reset();
}

void FeedbackEngine::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    ringPos = 0;
    hopCounter = 0;

    for (auto& t : tracks) t = Track {};
    for (auto& n : notches) n = Notch {};

    displayLocal.fill (-120.0f);
    for (auto& d : displayDb) d.store (-120.0f);
    for (auto& a : uiActive) a.store (false);
    numActiveNotches.store (0);
}

//==============================================================================
void FeedbackEngine::computeDerived (const EngineParams& p, int subBlockLen)
{
    const float s = juce::jlimit (0.0f, 1.0f, p.sensitivity);
    const float v = juce::jlimit (0.0f, 1.0f, p.speed);

    dp.gateDb  = lerp (-40.0f, -78.0f, s);
    dp.paprThr = lerp (24.0f, 9.0f, s);
    dp.pnprThr = lerp (32.0f, 16.0f, s);
    dp.devThr  = lerp (0.18f, 0.45f, s);

    const float needMs = lerp (260.0f, 35.0f, v);
    dp.needFrames = juce::jmax (2.0f, needMs * 0.001f / frameSeconds());

    const float attackTau = lerp (0.030f, 0.003f, v);
    dp.attackCoef = 1.0f - std::exp (-(float) subBlockLen / (attackTau * (float) sampleRate));

    dp.maxDepth = juce::jlimit (3.0f, 60.0f, p.maxDepthDb);
    dp.releaseStepDb = dp.maxDepth * frameSeconds() / juce::jmax (0.05f, p.releaseSeconds);
    dp.q = juce::jlimit (1.0f, 200.0f, p.q);
    dp.maxNotches = juce::jlimit (1, kMaxNotches, p.maxNotches);
    dp.lock = p.lock;

    const float lo = juce::jlimit (20.0f, (float) sampleRate * 0.45f, p.minHz);
    const float hi = juce::jlimit (lo + 10.0f, (float) sampleRate * 0.48f, p.maxHz);
    dp.kLo = juce::jmax (3, (int) std::ceil (lo / binHz));
    dp.kHi = juce::jmin (fftSize / 2 - 14, (int) std::floor (hi / binHz));
}

//==============================================================================
void FeedbackEngine::process (float* const* channels, int numChannels, int numSamples, const EngineParams& params)
{
    if (fft == nullptr || numSamples <= 0)
        return;

    numChannels = juce::jmin (numChannels, kMaxChannels);
    computeDerived (params, kSubBlock);

    // If the user lowered the filter count, release the shallowest surplus notches.
    {
        int active = 0;
        for (auto& n : notches) if (n.active && ! n.forceRelease) ++active;

        while (active > dp.maxNotches)
        {
            Notch* shallowest = nullptr;
            for (auto& n : notches)
                if (n.active && ! n.forceRelease && (shallowest == nullptr || n.targetDepth < shallowest->targetDepth))
                    shallowest = &n;

            shallowest->forceRelease = true;
            shallowest->holdFrames = 0;
            --active;
        }
    }

    const float monoScale = 1.0f / (float) juce::jmax (1, numChannels);
    const int ringMask = fftSize - 1;

    for (int pos = 0; pos < numSamples; pos += kSubBlock)
    {
        const int len = juce::jmin (kSubBlock, numSamples - pos);

        // 1) feed the analyser with the (unfiltered) input
        for (int i = 0; i < len; ++i)
        {
            float mono = 0.0f;
            for (int c = 0; c < numChannels; ++c)
                mono += channels[c][pos + i];

            ring[(size_t) ringPos] = mono * monoScale;
            ringPos = (ringPos + 1) & ringMask;

            if (++hopCounter >= hopSize)
            {
                hopCounter = 0;
                analyseFrame();
                updateNotchesPerFrame();
            }
        }

        // 2) glide notch depth / frequency and refresh coefficients
        updateNotchCoefficients (len);

        // 3) run the notch bank
        for (auto& n : notches)
        {
            if (n.curDepth < 0.01f && ! n.active)
                continue;

            const float a1 = n.a1, a2 = n.a2, a3 = n.a3, m1 = n.m1;

            for (int c = 0; c < numChannels; ++c)
            {
                float* x = channels[c] + pos;
                float ic1 = n.ic1[(size_t) c], ic2 = n.ic2[(size_t) c];

                for (int i = 0; i < len; ++i)
                {
                    const float v0 = x[i];
                    const float v3 = v0 - ic2;
                    const float v1 = a1 * ic1 + a2 * v3;
                    const float v2 = ic2 + a2 * ic1 + a3 * v3;
                    ic1 = 2.0f * v1 - ic1;
                    ic2 = 2.0f * v2 - ic2;
                    x[i] = v0 + m1 * v1;
                }

                n.ic1[(size_t) c] = ic1;
                n.ic2[(size_t) c] = ic2;
            }
        }
    }

    // publish notch state for the UI
    int active = 0;
    for (size_t i = 0; i < notches.size(); ++i)
    {
        const auto& n = notches[i];
        const bool visible = n.active && n.curDepth > 0.05f;
        uiActive[i].store (visible);
        uiFreq[i].store (n.curFreq);
        uiDepth[i].store (n.curDepth);
        uiQ[i].store (n.qEff);
        if (visible) ++active;
    }
    numActiveNotches.store (active);
}

//==============================================================================
void FeedbackEngine::analyseFrame()
{
    // ---- windowed STFT frame, oldest sample first --------------------------------------
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = ring[(size_t) ((ringPos + i) & (fftSize - 1))] * window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

    fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

    // normalise so that a full-scale sine reads 0 dBFS (Hann coherent gain = 0.5)
    const int numBins = fftSize / 2 + 1;
    const float norm = 4.0f / (float) fftSize;

    prefix[0] = 0.0f;
    for (int k = 0; k < numBins; ++k)
    {
        const float mag = fftData[(size_t) k] * norm;
        power[(size_t) k]   = mag * mag;
        powerDb[(size_t) k] = toDb (mag * mag);
        prefix[(size_t) k + 1] = prefix[(size_t) k] + power[(size_t) k];
    }

    publishDisplay();

    // ---- band statistics --------------------------------------------------------------
    const int kLo = dp.kLo, kHi = dp.kHi;
    if (kHi <= kLo + 4)
        return;

    double bandSum = 0.0;
    for (int k = kLo; k <= kHi; ++k) bandSum += power[(size_t) k];
    const float avgDb = toDb ((float) (bandSum / (kHi - kLo + 1)));

    auto rangeMean = [this, numBins] (int a, int b)
    {
        a = juce::jlimit (1, numBins - 1, a);
        b = juce::jlimit (1, numBins - 1, b);
        if (b < a) return 0.0f;
        return (prefix[(size_t) b + 1] - prefix[(size_t) a]) / (float) (b - a + 1);
    };

    // ---- local maxima + peak-to-neighbour ratio for every bin ---------------------------
    peaks.clear();
    std::fill (isLocalMax.begin(), isLocalMax.end(), (unsigned char) 0);

    for (int k = 3; k < numBins - 14; ++k)
    {
        const float p = power[(size_t) k];
        if (! (p > power[(size_t) k - 1] && p >= power[(size_t) k + 1]
               && p > power[(size_t) k - 2] && p >= power[(size_t) k + 2]))
            continue;

        // neighbourhood outside the Hann main lobe: bins k±4 .. k±12
        const float neigh = 0.5f * (rangeMean (k - 12, k - 4) + rangeMean (k + 4, k + 12));
        const float pnpr = powerDb[(size_t) k] - toDb (neigh);
        pnprByBin[(size_t) k] = pnpr;
        isLocalMax[(size_t) k] = 1;

        if (k >= kLo && k <= kHi && powerDb[(size_t) k] > dp.gateDb && pnpr > dp.pnprThr - 8.0f)
            peaks.push_back ({ k, powerDb[(size_t) k], pnpr });
    }

    // only consider the loudest candidates
    constexpr size_t kMaxPeaks = 24;
    if (peaks.size() > kMaxPeaks)
    {
        std::partial_sort (peaks.begin(), peaks.begin() + (long) kMaxPeaks, peaks.end(),
                           [] (const Peak& a, const Peak& b) { return a.db > b.db; });
        peaks.resize (kMaxPeaks);
    }

    for (auto& t : tracks) t.seen = false;

    // ---- score every candidate and associate it with a track -----------------------------
    for (const auto& pk : peaks)
    {
        const int k = pk.bin;
        const float a = powerDb[(size_t) k - 1], b = powerDb[(size_t) k], c = powerDb[(size_t) k + 1];
        const float denom = a - 2.0f * b + c;
        const float delta = std::abs (denom) > 1.0e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;
        const float binF = (float) k + delta;
        const float peakDb = b - 0.25f * (a - c) * delta;

        const float papr = peakDb - avgDb;
        const bool harmonic = isHarmonicPartial (binF, peakDb);

        // find nearest track
        Track* match = nullptr;
        float bestDist = juce::jmax (1.2f, binF * 0.01f);
        for (auto& t : tracks)
        {
            if (! t.used || t.seen) continue;
            const float d = std::abs (t.bin - binF);
            if (d < bestDist) { bestDist = d; match = &t; }
        }

        if (match == nullptr)
        {
            for (auto& t : tracks)
                if (! t.used) { match = &t; break; }

            if (match == nullptr)
            {
                // recycle the weakest track
                match = &tracks[0];
                for (auto& t : tracks)
                    if (t.conf < match->conf) match = &t;
            }

            *match = Track {};
            match->used   = true;
            match->bin    = binF;
            match->devEma = 0.5f;          // unknown stability until proven
            match->lastDb = peakDb;
        }
        else
        {
            match->devEma   = 0.7f * match->devEma + 0.3f * std::abs (binF - match->bin);
            match->slopeEma = 0.6f * match->slopeEma + 0.4f * (peakDb - match->lastDb);
            match->bin      = binF;
            match->lastDb   = peakDb;
        }

        match->seen = true;
        match->missed = 0;

        // A howl never fades on its own: a decaying partial is a voice/instrument note ending.
        const bool feedbackLike = papr > dp.paprThr
                               && pk.pnpr > dp.pnprThr
                               && match->devEma < dp.devThr
                               && match->slopeEma > -0.4f
                               && ! harmonic;

        if (feedbackLike)
        {
            float gain = 1.0f;
            if (match->slopeEma > 0.5f && match->devEma < 0.5f * dp.devThr)  gain += 0.5f;   // steady exponential build-up
            if (pk.pnpr > dp.pnprThr + 15.0f)                                gain += 1.0f;   // extremely pure tone
            match->conf += gain;
        }
        else
        {
            match->conf -= 1.5f;
        }

        match->conf = juce::jlimit (0.0f, dp.needFrames * 2.0f, match->conf);

        // only act on frames that themselves look like feedback
        if (feedbackLike && match->conf >= dp.needFrames)
            onFeedbackDetected (binF * binHz, pk.pnpr);
    }

    for (auto& t : tracks)
    {
        if (! t.used || t.seen) continue;
        t.conf = juce::jmax (0.0f, t.conf - 2.0f);
        if (++t.missed > 3) t = Track {};
    }
}

//==============================================================================
float FeedbackEngine::prominentPeakNear (float bin, float tolBins) const
{
    const int numBins = fftSize / 2 + 1;
    const int a = juce::jmax (3, (int) std::floor (bin - tolBins));
    const int b = juce::jmin (numBins - 15, (int) std::ceil (bin + tolBins));

    float best = -200.0f;
    for (int k = a; k <= b; ++k)
        if (isLocalMax[(size_t) k] && pnprByBin[(size_t) k] > 8.0f)
            best = juce::jmax (best, powerDb[(size_t) k]);
    return best;
}

bool FeedbackEngine::isHarmonicPartial (float bin, float peakDb) const
{
    // Test whether this peak belongs to a harmonic series (voice / instrument): assume it is
    // harmonic n of some fundamental f0 = f/n and look for the partials such a series must have.
    // Feedback howls are (nearly) pure sinusoids, so they rarely satisfy this. Tolerances are
    // kept tight so that the dense partials of a voice underneath a howl don't match by chance.
    const float maxBin = (float) (fftSize / 2 - 16);
    const float minF0Bin = 70.0f / binHz;
    const float maxF0Bin = 1000.0f / binHz;
    const float floorDb = juce::jmax (peakDb - 30.0f, dp.gateDb - 10.0f);

    auto present = [&] (float target)
    {
        return target <= maxBin && prominentPeakNear (target, juce::jmax (1.0f, target * 0.01f)) > floorDb;
    };

    for (int n = 1; n <= 10; ++n)
    {
        const float f0 = bin / (float) n;
        if (f0 < minF0Bin) break;
        if (f0 > maxF0Bin) continue;

        if (n == 1)
        {
            // As a fundamental it needs overtones. Symmetric clipping of a howl only adds odd
            // partials, so a clear 2nd partial (or 3rd + 4th) means voice or instrument.
            if (present (2.0f * f0) || (present (3.0f * f0) && present (4.0f * f0)))
                return true;
        }
        else
        {
            // As an overtone, the fundamental and a neighbouring partial must both be there.
            const bool neighbour = (n > 2 && present ((float) (n - 1) * f0)) || present ((float) (n + 1) * f0);
            if (present (f0) && neighbour)
                return true;
        }
    }
    return false;
}

//==============================================================================
void FeedbackEngine::onFeedbackDetected (float freqHz, float pnpr)
{
    totalDetections.fetch_add (1);

    // enough cut to bring the peak back down towards its spectral neighbourhood
    const float estimate = juce::jlimit (6.0f, dp.maxDepth, pnpr - 10.0f);
    const int holdFrames = (int) std::ceil (1.0f / frameSeconds());   // 1 s hold

    // 1) existing notch close enough? -> retune & deepen
    for (auto& n : notches)
    {
        if (! n.active) continue;
        const float tol = juce::jmax (1.5f * binHz, n.freq * 0.015f);
        if (std::abs (n.freq - freqHz) < tol)
        {
            n.freq += 0.3f * (freqHz - n.freq);

            // still ringing although the notch is fully in -> escalate
            const bool settled = n.curDepth > n.targetDepth - 1.0f;
            n.targetDepth = juce::jmin (dp.maxDepth, juce::jmax (n.targetDepth + (settled ? 2.0f : 0.0f), estimate));
            n.holdFrames = holdFrames;
            n.forceRelease = false;
            return;
        }
    }

    // 2) allocate a free notch, or steal the shallowest one
    int activeCount = 0;
    for (auto& n : notches) if (n.active && ! n.forceRelease) ++activeCount;

    Notch* slot = nullptr;
    if (activeCount < dp.maxNotches)
    {
        for (auto& n : notches)
            if (! n.active && n.curDepth < 0.05f) { slot = &n; break; }
        if (slot == nullptr)
            for (auto& n : notches)
                if (! n.active) { slot = &n; break; }
    }

    if (slot == nullptr)
    {
        for (auto& n : notches)
        {
            if (! n.active) continue;
            const bool better = slot == nullptr
                             || (n.holdFrames == 0 && slot->holdFrames > 0)
                             || ((n.holdFrames == 0) == (slot->holdFrames == 0) && n.targetDepth < slot->targetDepth);
            if (better) slot = &n;
        }
    }

    if (slot == nullptr)
        return;

    if (slot->curDepth < 0.5f)
    {
        slot->curFreq = freqHz;
        slot->ic1.fill (0.0f);
        slot->ic2.fill (0.0f);
    }

    slot->active       = true;
    slot->forceRelease = false;
    slot->freq         = freqHz;
    slot->targetDepth  = estimate;
    slot->holdFrames   = holdFrames;
}

void FeedbackEngine::updateNotchesPerFrame()
{
    for (auto& n : notches)
    {
        if (! n.active) continue;

        if (n.forceRelease)
        {
            n.targetDepth = 0.0f;
        }
        else if (n.holdFrames > 0)
        {
            --n.holdFrames;
        }
        else if (! dp.lock)
        {
            n.targetDepth = juce::jmax (0.0f, n.targetDepth - dp.releaseStepDb);
        }

        n.targetDepth = juce::jmin (n.targetDepth, dp.maxDepth);

        if (n.targetDepth <= 0.0f && n.curDepth < 0.05f)
        {
            n.active = false;
            n.forceRelease = false;
        }
    }
}

void FeedbackEngine::updateNotchCoefficients (int numSamples)
{
    const float releaseCoef = 1.0f - std::exp (-(float) numSamples / (0.05f * (float) sampleRate));
    const float freqCoef    = 1.0f - std::exp (-(float) numSamples / (0.03f * (float) sampleRate));
    const float nyquistSafe = (float) sampleRate * 0.45f;

    for (auto& n : notches)
    {
        if (! n.active && n.curDepth < 0.01f)
        {
            n.curDepth = 0.0f;
            continue;
        }

        const float coef = n.targetDepth > n.curDepth ? dp.attackCoef : releaseCoef;
        n.curDepth += (n.targetDepth - n.curDepth) * coef;
        n.curFreq  += (n.freq - n.curFreq) * freqCoef;

        // never make the notch narrower than the frequency estimate can justify
        n.qEff = juce::jmin (dp.q, juce::jmax (2.0f, n.curFreq / (1.5f * binHz)));

        // Simper SVF bell (equivalent to the RBJ peaking EQ), gain = -curDepth dB
        const float A = std::pow (10.0f, -n.curDepth / 40.0f);
        const float g = std::tan (juce::MathConstants<float>::pi * juce::jmin (n.curFreq, nyquistSafe) / (float) sampleRate);
        const float k = 1.0f / (n.qEff * A);

        n.a1 = 1.0f / (1.0f + g * (g + k));
        n.a2 = g * n.a1;
        n.a3 = g * n.a2;
        n.m1 = k * (A * A - 1.0f);
    }
}

//==============================================================================
void FeedbackEngine::publishDisplay()
{
    const int numBins = fftSize / 2 + 1;
    const float ratio = kDisplayMaxHz / kDisplayMinHz;

    for (int i = 0; i < kDisplayPoints; ++i)
    {
        const float f0 = kDisplayMinHz * std::pow (ratio, (float) i / (float) kDisplayPoints);
        const float f1 = kDisplayMinHz * std::pow (ratio, (float) (i + 1) / (float) kDisplayPoints);
        const int b0 = juce::jlimit (1, numBins - 1, (int) std::floor (f0 / binHz));
        const int b1 = juce::jlimit (b0, numBins - 1, (int) std::floor (f1 / binHz));

        float m = -200.0f;
        for (int k = b0; k <= b1; ++k) m = juce::jmax (m, powerDb[(size_t) k]);

        auto& d = displayLocal[(size_t) i];
        d = m > d ? m : juce::jmax (m, d - 1.5f);    // fast attack, slow fall
        displayDb[(size_t) i].store (d);
    }
}

void FeedbackEngine::getDisplaySpectrum (float* destDb, int numPoints) const
{
    for (int i = 0; i < juce::jmin (numPoints, kDisplayPoints); ++i)
        destDb[i] = displayDb[(size_t) i].load();
}

std::array<FeedbackEngine::NotchSnapshot, FeedbackEngine::kMaxNotches> FeedbackEngine::getNotches() const
{
    std::array<NotchSnapshot, kMaxNotches> out;
    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i].active  = uiActive[i].load();
        out[i].freq    = uiFreq[i].load();
        out[i].depthDb = uiDepth[i].load();
        out[i].q       = uiQ[i].load();
    }
    return out;
}

} // namespace dfb
