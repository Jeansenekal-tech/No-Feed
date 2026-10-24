// Tests for the AI voice engine.
//
//   NoFeedAiTest <model.bin> [parity.bin] [voice.f32 (48 kHz mono float32, for the closed-loop test)]
//
//  1. C++ network matches PyTorch on real features (parity file from training/export.py)
//  2. amount = 0 is a perfect delayed reconstruction (asymmetric STFT is correct)
//  3. CPU cost per second of audio
//  4. Closed loop with real speech: mic -> NoFeed AI -> speaker -> room -> mic must not howl
//     at a loop gain that howls without it.

#include "AiVoiceEngine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <random>
#include <vector>

namespace
{
int failures = 0;
void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

std::vector<char> readFile (const char* path)
{
    std::ifstream f (path, std::ios::binary);
    return std::vector<char> ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
}

float rmsDb (const std::vector<float>& x, size_t a, size_t b)
{
    double s = 0; for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
    return 10.0f * (float) std::log10 (s / (double) (b - a) + 1e-20);
}
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: %s model.bin [parity.bin] [voice.f32]\n", argv[0]); return 2; }

    const auto modelData = readFile (argv[1]);
    dfb::VoiceMaskModel model;
    check (model.load (modelData.data(), modelData.size()), "model file loads");
    if (! model.isLoaded()) return 1;

    // ---- 1. parity with PyTorch ----------------------------------------------------------
    if (argc > 2)
    {
        const auto p = readFile (argv[2]);
        int frames = 0, bins = 0;
        std::memcpy (&frames, p.data(), 4); std::memcpy (&bins, p.data() + 4, 4);
        const float* feat = reinterpret_cast<const float*> (p.data() + 8);
        const float* ref  = feat + (size_t) frames * (size_t) bins;
        std::vector<float> mask ((size_t) bins);
        float maxErr = 0.0f;
        model.reset();
        for (int t = 0; t < frames; ++t)
        {
            model.forward (feat + (size_t) t * (size_t) bins, mask.data());
            for (int k = 0; k < bins; ++k)
                maxErr = std::max (maxErr, std::abs (mask[(size_t) k] - ref[(size_t) t * (size_t) bins + (size_t) k]));
        }
        std::printf ("  max |C++ - PyTorch| over %d frames: %.2e\n", frames, maxErr);
        check (maxErr < 1.0e-4f, "C++ network output matches PyTorch");
    }

    // ---- 2. reconstruction at amount 0 ----------------------------------------------------
    for (double sr : { 48000.0, 44100.0, 96000.0 })
    {
        dfb::AiVoiceEngine e;
        e.loadModel (modelData.data(), modelData.size());
        e.prepare (sr, 2);
        const int lat = e.getLatencySamples();
        std::mt19937 rng (1); std::normal_distribution<float> g (0.0f, 0.2f);
        std::vector<float> l ((size_t) sr), r ((size_t) sr);
        for (size_t i = 0; i < l.size(); ++i) { l[i] = g (rng); r[i] = g (rng); }
        auto L = l, R = r;
        for (size_t pos = 0; pos < L.size(); pos += 500)
        {
            float* ch[] = { L.data() + pos, R.data() + pos };
            e.process (ch, 2, (int) std::min<size_t> (500, L.size() - pos), 0.0f);
        }
        float err = 0.0f;
        for (size_t i = (size_t) lat; i < L.size(); ++i)
            err = std::max ({ err, std::abs (L[i] - l[i - (size_t) lat]), std::abs (R[i] - r[i - (size_t) lat]) });
        std::printf ("  %6.0f Hz: latency %d samples (%.2f ms), reconstruction error %.1e\n", sr, lat, 1000.0 * lat / sr, err);
        check (err < 1.0e-4f, "amount 0 reconstructs the input exactly");
    }

    // ---- 3. CPU ----------------------------------------------------------------------------
    {
        dfb::AiVoiceEngine e;
        e.loadModel (modelData.data(), modelData.size());
        e.prepare (48000.0, 1);
        std::vector<float> x (48000 * 5, 0.01f);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t pos = 0; pos < x.size(); pos += 256)
        {
            float* ch[] = { x.data() + pos };
            e.process (ch, 1, (int) std::min<size_t> (256, x.size() - pos), 1.0f);
        }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  CPU: %.1f %% of one core (48 kHz, hidden %d)\n", 100.0 * sec / 5.0, model.getHiddenSize());
        check (sec / 5.0 < 0.5, "runs well within real time");
    }

    // ---- 4. closed loop with real speech ---------------------------------------------------
    if (argc > 3)
    {
        const auto raw = readFile (argv[3]);
        std::vector<float> voice (raw.size() / 4);
        std::memcpy (voice.data(), raw.data(), voice.size() * 4);
        const size_t n = std::min<size_t> (voice.size(), 48000 * 20);
        voice.resize (n);

        // loop: 6 ms delay, speaker/room with two resonant modes and a reflection
        auto runLoop = [&] (bool useAi, float loopGainDb, float amount)
        {
            dfb::AiVoiceEngine e;
            e.loadModel (modelData.data(), modelData.size());
            e.prepare (48000.0, 1);
            std::vector<float> spk (n, 0.0f), out (n, 0.0f), mic (64);
            const float g = std::pow (10.0f, loopGainDb / 20.0f);
            float s1a = 0, s2a = 0, s1b = 0, s2b = 0;
            auto bp = [] (float x, float f, float q, float& ic1, float& ic2)
            {
                const float gg = std::tan (3.14159265f * f / 48000.0f), k = 1.0f / q;
                const float a1 = 1.0f / (1.0f + gg * (gg + k)), a2 = gg * a1, a3 = gg * a2;
                const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2; return v1 * k;
            };
            const size_t d = 288;
            for (size_t pos = 0; pos < n; pos += 64)
            {
                const size_t len = std::min<size_t> (64, n - pos);
                for (size_t i = 0; i < len; ++i)
                {
                    const size_t t = pos + i;
                    const float s  = t >= d ? spk[t - d] : 0.0f;
                    const float s2 = t >= d + 700 ? spk[t - d - 700] : 0.0f;
                    const float leak = 0.35f * s + 0.75f * bp (s, 1250.0f, 7.0f, s1a, s2a) + 0.6f * bp (s, 2900.0f, 9.0f, s1b, s2b) + 0.2f * s2;
                    mic[i] = std::max (-1.0f, std::min (1.0f, voice[t] + g * leak));
                }
                float* ch[] = { mic.data() };
                if (useAi) e.process (ch, 1, (int) len, amount);
                for (size_t i = 0; i < len; ++i)
                {
                    out[pos + i] = mic[i];
                    spk[pos + i] = std::tanh (2.0f * mic[i]) / 2.0f;
                }
            }
            return out;
        };

        const size_t a = n / 2, b = n;
        const float voiceDb = rmsDb (voice, a, b);
        std::printf ("  closed loop, real held-out speech (voice %.1f dBFS):\n", voiceDb);
        for (float gdb : { 2.0f, 5.0f, 8.0f })
        {
            const auto off = runLoop (false, gdb, 0.0f);
            const auto on  = runLoop (true, gdb, 1.0f);
            std::printf ("    loop gain +%.0f dB: without AI %.1f dBFS, with AI %.1f dBFS\n", gdb, rmsDb (off, a, b), rmsDb (on, a, b));
            if (std::abs (gdb - 5.0f) < 0.01f)
            {
                check (rmsDb (off, a, b) > voiceDb + 6.0f, "without the AI the loop howls");
                check (rmsDb (on, a, b) < voiceDb + 3.0f, "with the AI the loop stays at voice level");
            }
        }
    }

    std::printf (failures == 0 ? "\nALL AI TESTS PASSED\n" : "\n%d AI TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
