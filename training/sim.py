"""Closed-loop acoustic feedback simulator used to generate training data.

mic[n] = voice_at_mic[n] + noise[n] + (loop_ir * speaker)[n]
speaker[n] = saturate(mic[n])

The loop impulse response = delay + random loudspeaker colouration + random room (early
reflections + reverberant tail), scaled to a random peak loop gain that ranges from stable
but ringing (-6 dB) to clearly unstable (+3.5 dB, howling). The training target is the dry
voice only, so the network learns to remove feedback, the loop's re-emission of the voice,
room reverb and noise.
"""
import os
import numpy as np
import scipy.signal as ss

SR = 48000


def peaking(f0, gain_db, q):
    a = 10 ** (gain_db / 40)
    w = 2 * np.pi * f0 / SR
    alpha = np.sin(w) / (2 * q)
    b = np.array([1 + alpha * a, -2 * np.cos(w), 1 - alpha * a])
    den = np.array([1 + alpha / a, -2 * np.cos(w), 1 - alpha / a])
    return b / den[0], den / den[0]


def speaker_ir(rng, n=1024):
    x = np.zeros(n); x[0] = 1.0
    b, a = ss.butter(2, rng.uniform(60, 220) / (SR / 2), "high"); x = ss.lfilter(b, a, x)
    b, a = ss.butter(2, rng.uniform(6000, 16000) / (SR / 2), "low"); x = ss.lfilter(b, a, x)
    for _ in range(rng.integers(2, 6)):
        b, a = peaking(np.exp(rng.uniform(np.log(150), np.log(9000))), rng.uniform(-9, 9), rng.uniform(0.7, 5))
        x = ss.lfilter(b, a, x)
    return x


def room_ir(rng, n):
    h = np.zeros(n); h[0] = 1.0
    for _ in range(rng.integers(3, 14)):                       # early reflections
        k = int(rng.uniform(0.001, 0.035) * SR)
        if k < n: h[k] += rng.uniform(0.15, 0.7) * rng.choice([-1, 1])
    rt60 = rng.uniform(0.2, 1.2)
    t = np.arange(n) / SR
    start = int(rng.uniform(0.004, 0.02) * SR)
    tail = rng.standard_normal(n) * np.exp(-6.9 * t / rt60)
    tail[:start] = 0
    b, a = ss.butter(1, rng.uniform(3000, 12000) / (SR / 2), "low")  # air absorption
    tail = ss.lfilter(b, a, tail)
    drr = 10 ** (rng.uniform(-6, 8) / 10)                     # direct-to-reverberant ratio
    tail *= np.sqrt(np.sum(h ** 2) / (drr * np.sum(tail ** 2) + 1e-12))
    return h + tail


def loop_ir(rng, total=4096):
    d = int(rng.uniform(0.002, 0.03) * SR)                    # acoustic delay 2..30 ms
    body = np.convolve(speaker_ir(rng), room_ir(rng, total))[: total - d]
    h = np.concatenate([np.zeros(d), body])
    H = np.abs(np.fft.rfft(h, 16384))
    f = np.fft.rfftfreq(16384, 1 / SR)
    peak = H[(f > 100) & (f < 12000)].max()
    return h / peak, d


def colored_noise(rng, n):
    kind = rng.integers(0, 4)
    w = rng.standard_normal(n)
    if kind == 1:   # pink-ish
        b, a = [0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400]
        w = ss.lfilter(b, a, w)
    elif kind == 2:  # brown-ish
        w = ss.lfilter([1], [1, -0.995], w)
    elif kind == 3:  # mains hum + harmonics + some hiss
        f0 = rng.choice([50, 60]); t = np.arange(n) / SR
        w = sum(rng.uniform(0.1, 1) / k * np.sin(2 * np.pi * f0 * k * t + rng.uniform(0, 6.28)) for k in range(1, 12)) + 0.05 * w
    return w / (np.std(w) + 1e-12)


def synth_singing(rng, n):
    """Harmonic 'sung' notes: held pitches with and without vibrato, plus formants.
    Teaches the network that sustained *harmonic* tones are voice, unlike feedback."""
    out = np.zeros(n); pos = 0
    vowels = [(730, 1090, 2440), (270, 2290, 3010), (530, 1840, 2480), (570, 840, 2410), (300, 870, 2240)]
    while pos < n:
        L = int(rng.uniform(0.25, 1.6) * SR); L = min(L, n - pos)
        f0 = np.exp(rng.uniform(np.log(90), np.log(800)))
        t = np.arange(L) / SR
        vib = rng.uniform(0, 0.025) * np.sin(2 * np.pi * rng.uniform(4.5, 6.5) * t)
        jitter = np.cumsum(rng.standard_normal(L)) * 2e-6
        ph = 2 * np.pi * np.cumsum(f0 * (1 + vib + jitter)) / SR
        tilt = rng.uniform(0.8, 1.6)
        x = sum(np.sin(k * ph) / k ** tilt for k in range(1, int(9000 / f0)))
        F = vowels[rng.integers(len(vowels))]
        y = np.zeros(L)
        for fc, g in zip(F, (1.0, 0.6, 0.3)):
            b, a = ss.iirpeak(fc, fc / 80, SR); y += g * ss.lfilter(b, a, x)
        env = np.minimum(1, np.minimum(t / 0.04, (L / SR - t) / 0.08))
        out[pos:pos + L] = y * np.clip(env, 0, 1)
        pos += L + int(rng.uniform(0, 0.3) * SR)
    return out[:n]


def make_example(rng, voices, n):
    # ---- voice --------------------------------------------------------------------------
    r = rng.random()
    if r < 0.05:
        voice = np.zeros(n)
    elif r < 0.17:
        voice = synth_singing(rng, n)
    else:
        v = voices[rng.integers(len(voices))]
        s = rng.integers(0, len(v) - n)
        voice = v[s:s + n].astype(np.float64) / 32767
    if np.any(voice):
        voice *= 10 ** (rng.uniform(-38, -12) / 20) / (np.sqrt(np.mean(voice ** 2)) + 1e-9)

    target = voice.copy()

    # mild room reverb on the voice itself (removed in the target)
    voice_at_mic = voice
    if rng.random() < 0.5 and np.any(voice):
        rir = room_ir(rng, int(rng.uniform(0.15, 0.6) * SR))
        rir[0] = 0
        rir *= 10 ** (rng.uniform(-22, -6) / 20) / (np.sqrt(np.sum(rir ** 2)) + 1e-9)
        voice_at_mic = voice + ss.fftconvolve(voice, rir)[:n]

    # ---- noise --------------------------------------------------------------------------
    noise = np.zeros(n)
    if rng.random() < 0.6:
        ref = np.sqrt(np.mean(voice ** 2)) if np.any(voice) else 10 ** (rng.uniform(-38, -12) / 20)
        noise = colored_noise(rng, n) * ref * 10 ** (-rng.uniform(5, 40) / 20)

    # ---- loop ---------------------------------------------------------------------------
    h, d = loop_ir(rng)
    g_db = rng.uniform(-8, 7)
    if rng.random() < 0.5:   # engineer pushing the fader during the clip
        gain = 10 ** (np.linspace(g_db - rng.uniform(3, 10), g_db, n) / 20)
    else:
        gain = np.full(n, 10 ** (g_db / 20))
    drive = rng.uniform(1, 4)                       # amp/speaker saturation point

    L = len(h); B = min(64, d)
    spkp = np.zeros(L + n)
    mic = np.zeros(n)
    src = voice_at_mic + noise
    for s0 in range(0, n, B):
        e = min(n, s0 + B)
        leak = np.convolve(spkp[s0 + 1: L + e], h, "valid")[: e - s0]
        m = np.clip(src[s0:e] + leak * gain[s0:e], -1, 1)     # ADC clip
        mic[s0:e] = m
        spkp[L + s0: L + e] = np.tanh(m * drive) / drive
    return mic.astype(np.float32), target.astype(np.float32), g_db
