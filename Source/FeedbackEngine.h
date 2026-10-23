#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace dfb
{

struct EngineParams
{
    float sensitivity    = 0.5f;     // 0..1, higher = catches quieter / less obvious feedback
    float speed          = 0.6f;     // 0..1, higher = reacts faster (less confirmation time)
    float releaseSeconds = 5.0f;     // time for a notch to fully release once feedback is gone
    float maxDepthDb     = 30.0f;    // deepest cut any single notch may apply
    float q              = 30.0f;    // notch quality (higher = narrower)
    int   maxNotches     = 12;       // how many simultaneous notches may be deployed
    float minHz          = 80.0f;    // detection range
    float maxHz          = 16000.0f;
    bool  lock           = false;    // when true, deployed notches never release
};

/**
    Real-time acoustic feedback suppressor.

    The engine continuously analyses the incoming signal with an STFT and scores every
    spectral peak against the classic feedback signatures (see van Waterschoot & Moonen,
    "Fifty years of acoustic feedback control", Proc. IEEE 2011):

      - PAPR  peak-to-average power ratio      (howls dominate the spectrum)
      - PNPR  peak-to-neighbour power ratio    (howls are pure, very narrow tones)
      - PHPR  harmonic structure               (voices/instruments have harmonics, howls don't)
      - frequency stability over time          (voices have jitter/vibrato, howls are locked)
      - level growth                           (howls build up exponentially)

    A peak that keeps matching these over several frames is classified as feedback and a
    dynamic notch (state-variable bell filter) is deployed on it, deepened while the feedback
    persists, and released slowly once it is gone. Processing is zero-latency: the filters are
    minimum-phase IIRs and detection runs on audio that has already been heard.
*/
class FeedbackEngine
{
public:
    static constexpr int kMaxNotches    = 16;
    static constexpr int kMaxChannels   = 8;
    static constexpr int kDisplayPoints = 256;
    static constexpr float kDisplayMinHz = 20.0f;
    static constexpr float kDisplayMaxHz = 20000.0f;

    struct NotchSnapshot
    {
        float freq    = 1000.0f;
        float depthDb = 0.0f;
        float q       = 30.0f;
        bool  active  = false;
    };

    FeedbackEngine();

    void prepare (double sampleRate, int numChannels);
    void reset();

    /** Filters the buffer in place. */
    void process (float* const* channels, int numChannels, int numSamples, const EngineParams& params);

    // ---- Thread-safe accessors for the UI -------------------------------------------------
    void getDisplaySpectrum (float* destDb, int numPoints) const;
    std::array<NotchSnapshot, kMaxNotches> getNotches() const;
    int getNumActiveNotches() const noexcept   { return numActiveNotches.load(); }
    int getTotalDetections() const noexcept    { return totalDetections.load(); }
    double getSampleRate() const noexcept      { return sampleRate; }

private:
    struct Notch
    {
        bool  active       = false;
        bool  forceRelease = false;
        float freq         = 1000.0f;
        float curFreq      = 1000.0f;
        float targetDepth  = 0.0f;   // dB of cut (positive)
        float curDepth     = 0.0f;
        int   holdFrames   = 0;
        float qEff         = 30.0f;

        // SVF bell coefficients
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, m1 = 0.0f;
        std::array<float, kMaxChannels> ic1 {}, ic2 {};
    };

    struct Track
    {
        bool  used     = false;
        bool  seen     = false;
        float bin      = 0.0f;   // fractional FFT bin
        float devEma   = 0.0f;   // smoothed frame-to-frame frequency deviation (bins)
        float lastDb   = -200.0f;
        float slopeEma = 0.0f;   // smoothed level growth (dB / frame)
        float conf     = 0.0f;   // accumulated evidence
        int   missed   = 0;
    };

    struct Peak
    {
        int   bin;
        float db;
        float pnpr;
    };

    struct DerivedParams
    {
        float gateDb, paprThr, pnprThr, devThr, needFrames;
        float attackCoef, releaseStepDb, maxDepth, q;
        int   maxNotches;
        int   kLo, kHi;
        bool  lock;
    };

    void computeDerived (const EngineParams&, int subBlockLen);
    void analyseFrame();
    void onFeedbackDetected (float freqHz, float pnpr);
    void updateNotchesPerFrame();
    void updateNotchCoefficients (int numSamples);
    void publishDisplay();

    float prominentPeakNear (float bin, float tolBins) const;
    bool  isHarmonicPartial (float bin, float peakDb) const;
    float frameSeconds() const noexcept { return (float) hopSize / (float) sampleRate; }

    double sampleRate = 48000.0;
    int numChannelsPrepared = 2;

    int fftOrder = 12, fftSize = 4096, hopSize = 512;
    float binHz = 48000.0f / 4096.0f;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, ring, fftData, power, powerDb, prefix, pnprByBin;
    std::vector<unsigned char> isLocalMax;
    std::vector<Peak> peaks;
    int ringPos = 0, hopCounter = 0;

    std::array<Track, 48> tracks;
    std::array<Notch, kMaxNotches> notches;
    DerivedParams dp {};

    // UI exchange
    std::array<float, kDisplayPoints> displayLocal {};
    std::array<std::atomic<float>, kDisplayPoints> displayDb;
    std::array<std::atomic<float>, kMaxNotches> uiFreq, uiDepth, uiQ;
    std::array<std::atomic<bool>, kMaxNotches> uiActive;
    std::atomic<int> numActiveNotches { 0 }, totalDetections { 0 };

    static constexpr int kSubBlock = 16;
};

} // namespace dfb
