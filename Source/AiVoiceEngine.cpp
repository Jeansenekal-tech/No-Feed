#include "AiVoiceEngine.h"

#include <cmath>
#include <cstring>

namespace dfb
{

//==============================================================================
// Model file: "NFM1", int32 bins, int32 hidden, then float32 arrays:
//   mean[bins] std[bins] | inp.W[H][bins] inp.b[H]
//   gru1.W_ih[3H][H] gru1.W_hh[3H][H] gru1.b_ih[3H] gru1.b_hh[3H] | gru2 (same)
//   out.W[bins][2H] out.b[bins]
// (PyTorch row-major layouts; GRU gate order r, z, n.)

namespace
{
    struct Reader
    {
        const unsigned char* p; size_t left;
        bool read (void* dst, size_t n) { if (n > left) return false; std::memcpy (dst, p, n); p += n; left -= n; return true; }
        bool floats (std::vector<float>& v, size_t n) { v.resize (n); return read (v.data(), n * sizeof (float)); }
    };

    // PyTorch [out][in] row-major -> [in][out] (column-major for y += W[:, j] * x[j])
    std::vector<float> transpose (const std::vector<float>& w, int out, int in)
    {
        std::vector<float> t ((size_t) in * (size_t) out);
        for (int o = 0; o < out; ++o)
            for (int i = 0; i < in; ++i)
                t[(size_t) i * (size_t) out + (size_t) o] = w[(size_t) o * (size_t) in + (size_t) i];
        return t;
    }

    inline float sigmoid (float x) { return 1.0f / (1.0f + std::exp (-x)); }
}

bool VoiceMaskModel::load (const void* data, size_t size)
{
    hidden = 0;
    Reader r { static_cast<const unsigned char*> (data), size };

    char magic[4];
    int32_t bins = 0, h = 0;
    if (! r.read (magic, 4) || std::memcmp (magic, "NFM1", 4) != 0) return false;
    if (! r.read (&bins, 4) || ! r.read (&h, 4) || bins != kBins || h <= 0 || h > 1024) return false;

    std::vector<float> stdv, w;
    const auto numBins = (size_t) bins;
    if (! r.floats (mean, numBins) || ! r.floats (stdv, numBins)) return false;
    invStd.resize (numBins);
    for (size_t i = 0; i < invStd.size(); ++i) invStd[i] = 1.0f / stdv[i];

    auto readDense = [&] (Dense& d, int numIn, int numOut)
    {
        d.in = numIn; d.out = numOut;
        if (! r.floats (w, (size_t) numIn * (size_t) numOut) || ! r.floats (d.b, (size_t) numOut)) return false;
        d.wT = transpose (w, numOut, numIn);
        return true;
    };
    auto readGru = [&] (Gru& g, int in, int hid)
    {
        g.in = in; g.hid = hid;
        const auto rows = (size_t) (3 * hid);
        if (! r.floats (w, rows * (size_t) in)) return false;
        g.wihT = transpose (w, 3 * hid, in);
        if (! r.floats (w, rows * (size_t) hid)) return false;
        g.whhT = transpose (w, 3 * hid, hid);
        return r.floats (g.bih, rows) && r.floats (g.bhh, rows);
    };

    if (! readDense (inp, bins, h) || ! readGru (gru1, h, h) || ! readGru (gru2, h, h) || ! readDense (out, 2 * h, bins))
        return false;
    if (r.left != 0)
        return false;

    x0.assign ((size_t) h, 0.0f);
    h1.assign ((size_t) h, 0.0f);
    h2.assign ((size_t) h, 0.0f);
    cat.assign ((size_t) (2 * h), 0.0f);
    gi.assign ((size_t) (3 * h), 0.0f);
    gh.assign ((size_t) (3 * h), 0.0f);
    hidden = h;
    return true;
}

void VoiceMaskModel::reset()
{
    std::fill (h1.begin(), h1.end(), 0.0f);
    std::fill (h2.begin(), h2.end(), 0.0f);
}

void VoiceMaskModel::matVecAdd (const std::vector<float>& wT, int in, int outN, const float* x, float* y)
{
    const float* col = wT.data();
    for (int j = 0; j < in; ++j, col += outN)
    {
        const float xj = x[j];
        if (! (xj > 0.0f || xj < 0.0f)) continue;   // ReLU outputs are often exactly zero
        for (int i = 0; i < outN; ++i)
            y[i] += col[i] * xj;
    }
}

void VoiceMaskModel::gruStep (const Gru& g, const float* x, std::vector<float>& h)
{
    const int H = g.hid;
    std::copy (g.bih.begin(), g.bih.end(), gi.begin());
    std::copy (g.bhh.begin(), g.bhh.end(), gh.begin());
    matVecAdd (g.wihT, g.in, 3 * H, x, gi.data());
    matVecAdd (g.whhT, H, 3 * H, h.data(), gh.data());

    for (int i = 0; i < H; ++i)
    {
        const float r = sigmoid (gi[(size_t) i] + gh[(size_t) i]);
        const float z = sigmoid (gi[(size_t) (H + i)] + gh[(size_t) (H + i)]);
        const float n = std::tanh (gi[(size_t) (2 * H + i)] + r * gh[(size_t) (2 * H + i)]);
        h[(size_t) i] = (1.0f - z) * n + z * h[(size_t) i];
    }
}

void VoiceMaskModel::forward (const float* logPower, float* mask)
{
    // input layer on normalised features
    float norm[kBins];
    for (int k = 0; k < kBins; ++k)
        norm[k] = (logPower[k] - mean[(size_t) k]) * invStd[(size_t) k];

    std::copy (inp.b.begin(), inp.b.end(), x0.begin());
    matVecAdd (inp.wT, inp.in, inp.out, norm, x0.data());
    for (auto& v : x0) v = v > 0.0f ? v : 0.0f;

    gruStep (gru1, x0.data(), h1);
    gruStep (gru2, h1.data(), h2);

    std::copy (h1.begin(), h1.end(), cat.begin());
    std::copy (h2.begin(), h2.end(), cat.begin() + hidden);

    std::copy (out.b.begin(), out.b.end(), mask);
    matVecAdd (out.wT, out.in, out.out, cat.data(), mask);
    for (int k = 0; k < kBins; ++k)
        mask[k] = sigmoid (mask[k]);
}

//==============================================================================
AiVoiceEngine::AiVoiceEngine()
{
    for (auto& d : dispIn)  d.store (-120.0f);
    for (auto& d : dispOut) d.store (-120.0f);
}

void AiVoiceEngine::prepare (double newSampleRate, int numChannels)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    numChannelsPrepared = juce::jlimit (1, kMaxChannels, numChannels);

    // keep the network's view constant: ~47 Hz bins and 375 frames/s at any sample rate
    scale = sampleRate < 70000.0 ? 1 : (sampleRate < 140000.0 ? 2 : 4);
    fftSize   = 1024 * scale;
    hop       = 128 * scale;
    synthHalf = 128 * scale;
    numBins   = fftSize / 2 + 1;

    int order = 0;
    while ((1 << order) < fftSize) ++order;
    fft = std::make_unique<juce::dsp::FFT> (order);

    // asymmetric windows (identical to training/train.py): analysis * synthesis = Hann(2M)
    // over the last 2M samples, which overlap-adds to exactly 1 at hop M.
    const int N = fftSize, M = synthHalf;
    anaWin.assign ((size_t) N, 0.0f);
    synWin.assign ((size_t) N, 0.0f);
    const double pi = juce::MathConstants<double>::pi;
    for (int n = 0; n < N; ++n)
    {
        if (n < N - M) anaWin[(size_t) n] = (float) std::sqrt (0.5 - 0.5 * std::cos (pi * n / (N - M)));
        else           anaWin[(size_t) n] = (float) std::sqrt (0.5 + 0.5 * std::cos (pi * (n - (N - M)) / M));
    }
    for (int j = 0; j < 2 * M; ++j)
    {
        const int n = N - 2 * M + j;
        const double hann = 0.5 - 0.5 * std::cos (pi * j / M);
        synWin[(size_t) n] = anaWin[(size_t) n] > 1.0e-9f ? (float) (hann / anaWin[(size_t) n]) : 0.0f;
    }

    frame.assign ((size_t) N * 2, 0.0f);
    monoSpec.assign ((size_t) N * 2, 0.0f);
    logPow.assign ((size_t) VoiceMaskModel::kBins, 0.0f);
    mask.assign ((size_t) VoiceMaskModel::kBins, 1.0f);
    gains.assign ((size_t) numBins, 1.0f);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        inRing[(size_t) c].assign ((size_t) N, 0.0f);
        outAccum[(size_t) c].assign ((size_t) N, 0.0f);
        chanSpec[(size_t) c].assign ((size_t) N * 2, 0.0f);
    }
    reset();
}

void AiVoiceEngine::reset()
{
    pos = 0;
    hopCounter = 0;
    for (int c = 0; c < kMaxChannels; ++c)
    {
        std::fill (inRing[(size_t) c].begin(), inRing[(size_t) c].end(), 0.0f);
        std::fill (outAccum[(size_t) c].begin(), outAccum[(size_t) c].end(), 0.0f);
    }
    model.reset();
    reductionDb.store (0.0f);
}

void AiVoiceEngine::process (float* const* channels, int numChannels, int numSamples, float amount)
{
    if (fft == nullptr)
        return;

    numChannels = juce::jmin (numChannels, numChannelsPrepared);
    amount = juce::jlimit (0.0f, 1.0f, amount);
    const int ringMask = fftSize - 1;

    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            const float x = channels[c][i];
            channels[c][i] = outAccum[(size_t) c][(size_t) pos];
            outAccum[(size_t) c][(size_t) pos] = 0.0f;
            inRing[(size_t) c][(size_t) pos] = x;
        }
        pos = (pos + 1) & ringMask;

        if (++hopCounter >= hop)
        {
            hopCounter = 0;
            processFrame (amount);
        }
    }
}

void AiVoiceEngine::processFrame (float amount)
{
    const int N = fftSize, M = synthHalf, ringMask = N - 1;
    const int nCh = numChannelsPrepared;

    // forward transform per channel (oldest sample at `pos`)
    for (int c = 0; c < nCh; ++c)
    {
        auto& spec = chanSpec[(size_t) c];
        const auto& ring = inRing[(size_t) c];
        for (int j = 0; j < N; ++j)
            spec[(size_t) j] = ring[(size_t) ((pos + j) & ringMask)] * anaWin[(size_t) j];
        std::fill (spec.begin() + N, spec.end(), 0.0f);
        fft->performRealOnlyForwardTransform (spec.data(), true);
    }

    // network sees the mid signal; one mask is applied to every channel
    const float monoScale = 1.0f / (float) nCh;
    for (int k = 0; k < numBins; ++k)
    {
        float re = 0.0f, im = 0.0f;
        for (int c = 0; c < nCh; ++c)
        {
            re += chanSpec[(size_t) c][(size_t) (2 * k)];
            im += chanSpec[(size_t) c][(size_t) (2 * k + 1)];
        }
        monoSpec[(size_t) (2 * k)]     = re * monoScale;
        monoSpec[(size_t) (2 * k + 1)] = im * monoScale;
    }

    const bool active = model.isLoaded() && amount > 0.0f;
    double inPow = 0.0, outPow = 0.0;

    if (model.isLoaded())
    {
        // at higher sample rates the FFT is `scale` times longer: bin k*scale of the long
        // transform is the same frequency as bin k at 48 kHz, and its power is scale^2 larger
        const float powScale = 1.0f / (float) (scale * scale);
        for (int k = 0; k < VoiceMaskModel::kBins; ++k)
        {
            const int kk = k * scale;
            const float re = monoSpec[(size_t) (2 * kk)], im = monoSpec[(size_t) (2 * kk + 1)];
            logPow[(size_t) k] = std::log10 ((re * re + im * im) * powScale + 1.0e-10f);
        }
        model.forward (logPow.data(), mask.data());   // always run so the state stays current
    }

    for (int k = 0; k < numBins; ++k)
    {
        float g = 1.0f;
        if (active)
        {
            // interpolate the 513-point mask onto this transform's bins
            const float pos01 = (float) k / (float) scale;
            const int k0 = juce::jmin ((int) pos01, VoiceMaskModel::kBins - 1);
            const int k1 = juce::jmin (k0 + 1, VoiceMaskModel::kBins - 1);
            const float f = pos01 - (float) k0;
            const float m = mask[(size_t) k0] + f * (mask[(size_t) k1] - mask[(size_t) k0]);
            g = 1.0f - amount * (1.0f - m);
        }
        gains[(size_t) k] = g;

        const float re = monoSpec[(size_t) (2 * k)], im = monoSpec[(size_t) (2 * k + 1)];
        const float p = re * re + im * im;
        inPow += p;
        outPow += p * g * g;
    }

    // apply, inverse transform, overlap-add the last 2M samples (latency = 2M)
    for (int c = 0; c < nCh; ++c)
    {
        auto& spec = chanSpec[(size_t) c];
        for (int k = 0; k < numBins; ++k)
        {
            spec[(size_t) (2 * k)]     *= gains[(size_t) k];
            spec[(size_t) (2 * k + 1)] *= gains[(size_t) k];
        }
        fft->performRealOnlyInverseTransform (spec.data());

        auto& acc = outAccum[(size_t) c];
        for (int j = 0; j < 2 * M; ++j)
        {
            const int n = N - 2 * M + j;
            acc[(size_t) ((pos + j) & ringMask)] += spec[(size_t) n] * synWin[(size_t) n];
        }
    }

    reductionDb.store (inPow > 1.0e-12 ? (float) (10.0 * std::log10 (inPow / juce::jmax (outPow, 1.0e-30))) : 0.0f);
    publishDisplay();
}

void AiVoiceEngine::publishDisplay()
{
    const float binHz = (float) sampleRate / (float) fftSize;
    const float norm = 4.0f / (float) fftSize;   // full-scale sine ~ 0 dB (approx. for this window)
    for (int i = 0; i < kDisplayPoints; ++i)
    {
        const float f0 = 20.0f * std::pow (1000.0f, (float) i / kDisplayPoints);
        const float f1 = 20.0f * std::pow (1000.0f, (float) (i + 1) / kDisplayPoints);
        const int b0 = juce::jlimit (1, numBins - 1, (int) (f0 / binHz));
        const int b1 = juce::jlimit (b0, numBins - 1, (int) (f1 / binHz));
        float pin = 0.0f, pout = 0.0f;
        for (int k = b0; k <= b1; ++k)
        {
            const float re = monoSpec[(size_t) (2 * k)], im = monoSpec[(size_t) (2 * k + 1)];
            const float p = (re * re + im * im) * norm * norm;
            pin  = juce::jmax (pin, p);
            pout = juce::jmax (pout, p * gains[(size_t) k] * gains[(size_t) k]);
        }
        const float din = 10.0f * std::log10 (pin + 1.0e-14f), dout = 10.0f * std::log10 (pout + 1.0e-14f);
        auto& li = dispInLocal[(size_t) i];
        auto& lo = dispOutLocal[(size_t) i];
        li = din > li ? din : juce::jmax (din, li - 0.6f);
        lo = dout > lo ? dout : juce::jmax (dout, lo - 0.6f);
        dispIn[(size_t) i].store (li);
        dispOut[(size_t) i].store (lo);
    }
}

void AiVoiceEngine::getDisplay (float* inDb, float* outDb, int numPoints) const
{
    for (int i = 0; i < juce::jmin (numPoints, kDisplayPoints); ++i)
    {
        inDb[i]  = dispIn[(size_t) i].load();
        outDb[i] = dispOut[(size_t) i].load();
    }
}

} // namespace dfb
