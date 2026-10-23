// Offline tests for the feedback engine.
//
//  1. Closed loop: a synthetic "singer" into a mic, the mic feeds a speaker through the engine,
//     and the speaker leaks back into the mic through a resonant room with enough gain to howl.
//  2. Open loop: a growing pure-tone howl mixed on top of the voice (like a recording).
//  3. Voice only: no feedback at all, to count false detections.

#include "FeedbackEngine.h"
#include "DeEsser.h"
#include "Denoiser.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;
constexpr float kTwoPi = 6.283185307f;

struct Svf   // simple TPT state-variable filter used to build the test signals
{
    float g = 0, k = 1, a1 = 1, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
    void set (float f, float q) { g = std::tan (3.14159265f * f / (float) kSr); k = 1.0f / q; a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2; }
    float band (float v0)
    {
        const float v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2; return v1 * k;   // unity gain at centre
    }
};

std::vector<float> makeVoice (double seconds, unsigned seed)
{
    const int n = (int) (seconds * kSr);
    std::vector<float> out ((size_t) n);
    std::mt19937 rng (seed);
    std::normal_distribution<float> noise (0.0f, 1.0f);

    const float vowels[][3] = { { 730, 1090, 2440 }, { 270, 2290, 3010 }, { 530, 1840, 2480 }, { 570, 840, 2410 }, { 300, 870, 2240 } };
    Svf f1, f2, f3;
    double phase = 0.0;
    float jitter = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const double t = i / kSr;
        const int syl = (int) (t * 3.5);
        const auto& v = vowels[(size_t) (syl * 7 + (int) seed) % 5];
        if (i % 64 == 0) { f1.set (v[0], 6); f2.set (v[1], 8); f3.set (v[2], 10); }

        jitter = 0.999f * jitter + 0.001f * noise (rng);
        const double f0 = (150.0 + 40.0 * std::sin (kTwoPi * 0.23 * t) + 25.0 * (syl % 3))
                        * (1.0 + 0.012 * std::sin (kTwoPi * 5.3 * t) + 0.02 * jitter);
        phase += f0 / kSr;
        phase -= std::floor (phase);

        float src = 0.0f;   // band-limited glottal-ish source
        for (int h = 1; h * f0 < 5000.0; ++h)
            src += std::sin (kTwoPi * (float) (h * phase)) / (float) h;

        const double sp = t * 3.5 - syl;  // syllable envelope with short gaps
        const float env = sp < 0.85 ? (float) std::pow (std::sin (3.14159 * sp / 0.85), 0.6) : 0.0f;
        const float voiced = 3.0f * f1.band (src) + 2.5f * f2.band (src) + 1.5f * f3.band (src);
        out[(size_t) i] = 0.12f * env * voiced + 0.002f * noise (rng);
    }
    return out;
}

float rms (const std::vector<float>& x, int a, int b)
{
    double s = 0; for (int i = a; i < b; ++i) s += (double) x[(size_t) i] * x[(size_t) i];
    return (float) std::sqrt (s / juce::jmax (1, b - a));
}

float toneLevelDb (const std::vector<float>& x, int a, int b, float freq)   // Goertzel
{
    const float w = kTwoPi * freq / (float) kSr, c = 2.0f * std::cos (w);
    double s1 = 0, s2 = 0;
    for (int i = a; i < b; ++i) { const double s0 = x[(size_t) i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 20.0f * std::log10 ((float) (2.0 * std::sqrt (p) / (b - a)) + 1.0e-9f);
}

// ---------------------------------------------------------------------------
struct LoopResult { float lateRms, peak; int notches, detections; };

LoopResult runClosedLoop (bool useEngine, const std::vector<float>& voice, const dfb::EngineParams& params)
{
    dfb::FeedbackEngine engine;
    engine.prepare (kSr, 1);

    const int n = (int) voice.size();
    const int delay = 260;                 // ~5.4 ms acoustic path
    const int block = 128;
    std::vector<float> speaker ((size_t) n, 0.0f), out ((size_t) n, 0.0f);

    // room: broadband loss + two resonant modes + an early reflection -> unstable at a few freqs
    Svf mode1, mode2;
    mode1.set (1180.0f, 6.0f);
    mode2.set (3150.0f, 7.0f);
    const float broadband = 0.3f, modeGain1 = 0.95f, modeGain2 = 0.85f, reflection = 0.1f;

    std::vector<float> mic ((size_t) block);
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        for (int i = 0; i < len; ++i)
        {
            const int idx = pos + i;
            const float s  = idx >= delay ? speaker[(size_t) (idx - delay)] : 0.0f;
            const float s2 = idx >= delay + 610 ? speaker[(size_t) (idx - delay - 610)] : 0.0f;
            const float leak = broadband * s + modeGain1 * mode1.band (s) + modeGain2 * mode2.band (s) + reflection * s2;
            mic[(size_t) i] = voice[(size_t) idx] + leak;
        }

        float* chans[] = { mic.data() };
        if (useEngine)
            engine.process (chans, 1, len, params);

        for (int i = 0; i < len; ++i)
        {
            const float y = std::tanh (mic[(size_t) i]);   // amp / speaker saturation
            out[(size_t) (pos + i)] = y;
            speaker[(size_t) (pos + i)] = y;
        }
    }

    if (useEngine)
    {
        std::printf ("  notches:");
        for (auto& nn : engine.getNotches()) if (nn.active) std::printf (" %.0fHz/%.0fdB", nn.freq, nn.depthDb);
        std::printf ("\n");
    }
    float peak = 0; for (int i = n / 2; i < n; ++i) peak = std::max (peak, std::abs (out[(size_t) i]));
    return { rms (out, n / 2, n), peak, engine.getNumActiveNotches(), engine.getTotalDetections() };
}

} // namespace

int main()
{
    int failures = 0;
    auto check = [&failures] (bool ok, const char* what)
    {
        std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (! ok) ++failures;
    };

    dfb::EngineParams params;   // defaults
    const auto voice = makeVoice (12.0, 1);
    const int n = (int) voice.size();
    const float voiceRms = rms (voice, n / 2, n);

    // ---- 1. closed loop ------------------------------------------------------------------
    std::printf ("Closed-loop feedback (mic -> engine -> speaker -> room -> mic)\n");
    const auto bypass = runClosedLoop (false, voice, params);
    const auto fixed  = runClosedLoop (true,  voice, params);
    std::printf ("  voice RMS %.3f | bypass RMS %.3f peak %.2f | engine RMS %.3f peak %.2f | notches %d, detections %d\n",
                 voiceRms, bypass.lateRms, bypass.peak, fixed.lateRms, fixed.peak, fixed.notches, fixed.detections);
    check (bypass.lateRms > 4.0f * voiceRms, "without the plugin the loop howls");
    check (fixed.lateRms < 2.0f * voiceRms, "with the plugin the loop is stable (output ~ voice level)");

    // ---- 2. open loop: howl on top of a recording -----------------------------------------
    std::printf ("Open-loop howl at 2470 Hz added to the voice from t=4s\n");
    {
        dfb::FeedbackEngine engine;
        engine.prepare (kSr, 2);
        std::vector<float> L (voice), R (voice);
        const float howlHz = 2470.0f;
        for (int i = 4 * (int) kSr; i < n; ++i)
        {
            const float t = (float) (i - 4 * kSr) / (float) kSr;
            const float amp = 0.25f * std::min (1.0f, std::pow (10.0f, (t - 1.0f) * 2.0f));  // rises 40 dB/s
            const float s = amp * std::sin (kTwoPi * howlHz * (float) i / (float) kSr);
            L[(size_t) i] += s; R[(size_t) i] += s;
        }
        const std::vector<float> inL (L);
        int notchesBeforeHowl = 0;
        for (int pos = 0; pos < n; pos += 256)
        {
            float* ch[] = { L.data() + pos, R.data() + pos };
            engine.process (ch, 2, std::min (256, n - pos), params);
            if (pos + 256 <= 4 * (int) kSr) notchesBeforeHowl = std::max (notchesBeforeHowl, engine.getNumActiveNotches());
        }
        const int a = 8 * (int) kSr, b = n;
        const float inDb = toneLevelDb (inL, a, b, howlHz), outDb = toneLevelDb (L, a, b, howlHz);
        std::printf ("  howl level in %.1f dBFS -> out %.1f dBFS (%.1f dB reduction); notches before howl: %d\n",
                     inDb, outDb, inDb - outDb, notchesBeforeHowl);
        check (inDb - outDb > 20.0f, "howl reduced by more than 20 dB");
    }

    // ---- 3. voice only: false positives --------------------------------------------------
    std::printf ("Voice only, no feedback (false-positive check)\n");
    {
        int maxNotches = 0;
        float maxDepth = 0.0f;
        for (unsigned seed = 1; seed <= 3; ++seed)
        {
            auto v = makeVoice (12.0, seed);
            dfb::FeedbackEngine engine;
            engine.prepare (kSr, 1);
            for (size_t pos = 0; pos < v.size(); pos += 512)
            {
                float* ch[] = { v.data() + pos };
                engine.process (ch, 1, (int) std::min<size_t> (512, v.size() - pos), params);
                maxNotches = std::max (maxNotches, engine.getNumActiveNotches());
                for (auto& nn : engine.getNotches())
                    if (nn.active && nn.depthDb > maxDepth) { maxDepth = nn.depthDb; std::printf ("  seed %u t=%.2fs notch %.0f Hz %.1f dB\n", seed, pos / kSr, nn.freq, nn.depthDb); }
            }
        }
        std::printf ("  max simultaneous notches %d, deepest %.1f dB\n", maxNotches, maxDepth);
        check (maxNotches <= 1 && maxDepth < 12.0f, "voice alone triggers no significant notching");
    }

    // ---- 4. de-noiser ---------------------------------------------------------------------
    std::printf ("De-noiser: voice + steady noise at -40 dBFS\n");
    {
        std::mt19937 rng (7);
        std::normal_distribution<float> gauss (0.0f, 1.0f);

        // transparency at amount 0: output == input delayed by the reported latency
        dfb::Denoiser dn;
        dn.prepare (kSr, 1);
        const int lat = dn.getLatencySamples();
        std::vector<float> x (48000), y;
        for (auto& v : x) v = 0.3f * gauss (rng);
        y = x;
        for (size_t pos = 0; pos < y.size(); pos += 480)
        {
            float* ch[] = { y.data() + pos };
            dn.process (ch, 1, 480, 0.0f);
        }
        float maxErr = 0.0f;
        for (size_t i = (size_t) lat; i < y.size(); ++i) maxErr = std::max (maxErr, std::abs (y[i] - x[i - (size_t) lat]));
        std::printf ("  latency %d samples, reconstruction error at amount 0: %.2e\n", lat, maxErr);
        check (maxErr < 1.0e-4f, "amount 0 is transparent (perfect reconstruction)");

        auto noisy = makeVoice (10.0, 2);
        const auto clean = noisy;
        for (auto& v : noisy) v += 0.01f * gauss (rng);
        const auto in = noisy;

        dfb::Denoiser dn2;
        dn2.prepare (kSr, 1);
        for (size_t pos = 0; pos < noisy.size(); pos += 256)
        {
            float* ch[] = { noisy.data() + pos };
            dn2.process (ch, 1, (int) std::min<size_t> (256, noisy.size() - pos), 0.7f);
        }

        // compare pauses (noise only) and vowel centres, compensating the latency
        double gapIn = 0, gapOut = 0, vIn = 0, vOut = 0;
        for (size_t i = 2 * (size_t) kSr; i + (size_t) lat < noisy.size(); ++i)
        {
            const double sp = std::fmod (i / kSr * 3.5, 1.0);
            const float o = noisy[i + (size_t) lat];
            if (sp > 0.9 && sp < 0.98)       { gapIn += in[i] * in[i]; gapOut += o * o; }
            else if (sp > 0.3 && sp < 0.55)  { vIn += clean[i] * clean[i]; vOut += o * o; }
        }
        const float gapRed = 10.0f * std::log10 ((float) (gapIn / gapOut));
        const float voiceChange = 10.0f * std::log10 ((float) (vOut / vIn));
        std::printf ("  noise in pauses reduced by %.1f dB, vowel level change %.2f dB\n", gapRed, voiceChange);
        check (gapRed > 10.0f, "noise in pauses reduced by more than 10 dB");
        check (std::abs (voiceChange) < 1.5f, "voice level preserved within 1.5 dB");
    }

    // ---- 5. de-esser ----------------------------------------------------------------------
    std::printf ("De-esser: voice + 80 ms sibilant bursts every 0.5 s\n");
    {
        std::mt19937 rng (3);
        std::normal_distribution<float> gauss (0.0f, 1.0f);
        auto v = makeVoice (8.0, 3);
        std::vector<char> isS (v.size(), 0);
        Svf sib; sib.set (7000.0f, 1.2f);
        for (size_t i = 0; i < v.size(); ++i)
        {
            const double ph = std::fmod (i / kSr, 0.5);
            const float nz = sib.band (gauss (rng));
            if (ph < 0.08) { v[i] += 0.25f * nz * (float) std::sin (3.14159 * ph / 0.08); isS[i] = ph > 0.02 && ph < 0.06; }
        }

        auto run = [&v] (float amount)
        {
            auto y = v;
            dfb::DeEsser de;
            de.prepare (kSr);
            for (size_t pos = 0; pos < y.size(); pos += 256)
            {
                float* ch[] = { y.data() + pos };
                de.process (ch, 1, (int) std::min<size_t> (256, y.size() - pos), amount, 5000.0f);
            }
            return y;
        };

        const auto off = run (0.0f);
        float maxDiff = 0.0f;
        for (size_t i = 0; i < v.size(); ++i) maxDiff = std::max (maxDiff, std::abs (off[i] - v[i]));
        check (maxDiff < 1.0e-6f, "amount 0 is bit-transparent");

        const auto on = run (0.7f);
        // sibilance measured in its own band (the voice underneath dominates a full-band reading)
        Svf measIn, measOut;
        measIn.set (7000.0f, 1.2f);
        measOut.set (7000.0f, 1.2f);
        double sIn = 0, sOut = 0, oIn = 0, oOut = 0;
        for (size_t i = 0; i < v.size(); ++i)
        {
            const double ph = std::fmod (i / kSr, 0.5);
            const float bi = measIn.band (v[i]), bo = measOut.band (on[i]);
            if (isS[i])                     { sIn += bi * bi; sOut += bo * bo; }
            else if (ph > 0.15 && ph < 0.45) { oIn += v[i] * v[i]; oOut += on[i] * on[i]; }
        }
        const float sRed = 10.0f * std::log10 ((float) (sIn / sOut));
        const float oChange = 10.0f * std::log10 ((float) (oOut / oIn));
        std::printf ("  sibilance band (7 kHz) reduced by %.1f dB, voiced passages changed by %.2f dB\n", sRed, oChange);
        check (sRed > 5.0f, "sibilance reduced by more than 5 dB");
        check (std::abs (oChange) < 0.5f, "non-sibilant voice left (almost) untouched");
    }

    std::printf (failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
