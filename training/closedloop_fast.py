"""Batched closed-loop data generation: K feedback loops run in lock-step, one hop at a time,
with a single batched network call per hop (the per-frame network call dominated before).

Each scene: voice/noise/room/loop as in sim.py, NoFeed's current network inside the loop
(streaming, identical to the plugin: asymmetric STFT, hop 128, latency 256). The recorded mic
signal is what the network actually faces once it is in the loop.
"""
import glob, os, sys
import numpy as np
import torch
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sim, train as T

ROOT = os.environ.get("NOFEED_AI_DIR", os.path.expanduser("~/nofeed-ai"))
TAG = os.environ.get("NOFEED_ROUND", "fast_")
N = 3 * 48000
K = 25                       # scenes per batch
HOP, NF, M = T.HOP, T.N_FFT, T.M


def scene(rng, voices):
    r = rng.random()
    if r < 0.05: voice = np.zeros(N)
    elif r < 0.17: voice = sim.synth_singing(rng, N)
    else:
        v = voices[rng.integers(len(voices))]; s = rng.integers(0, len(v) - N)
        voice = v[s:s + N].astype(np.float64) / 32767
    if np.any(voice):
        voice *= 10 ** (rng.uniform(-38, -12) / 20) / (np.sqrt(np.mean(voice ** 2)) + 1e-9)
    target = voice.copy()
    src = voice
    if rng.random() < 0.5 and np.any(voice):
        rir = sim.room_ir(rng, int(rng.uniform(0.15, 0.6) * sim.SR)); rir[0] = 0
        rir *= 10 ** (rng.uniform(-22, -6) / 20) / (np.sqrt(np.sum(rir ** 2)) + 1e-9)
        src = voice + sim.ss.fftconvolve(voice, rir)[:N]
    if rng.random() < 0.6:
        ref = np.sqrt(np.mean(voice ** 2)) if np.any(voice) else 10 ** (rng.uniform(-38, -12) / 20)
        src = src + sim.colored_noise(rng, N) * ref * 10 ** (-rng.uniform(5, 40) / 20)

    while True:
        h, d = sim.loop_ir(rng)
        if d >= HOP: break                      # lock-step needs delay >= one hop (2.7 ms)
    g_db = rng.uniform(0, 8)
    if rng.random() < 0.5:
        gain = 10 ** (np.linspace(g_db - rng.uniform(3, 10), g_db, N) / 20)
    else:
        gain = np.full(N, 10 ** (g_db / 20))
    return src, target, h, gain, rng.uniform(1, 4), g_db


@torch.no_grad()
def run_batch(rng, voices, net):
    sc = [scene(rng, voices) for _ in range(K)]
    L = max(len(s[2]) for s in sc)
    H = np.zeros((K, L));
    for k, s in enumerate(sc): H[k, :len(s[2])] = s[2]
    src = np.stack([s[0] for s in sc]); gain = np.stack([s[3] for s in sc]); drive = np.array([s[4] for s in sc])[:, None]

    wa = T.analysis_window(); ws = np.zeros(NF)
    j = np.arange(2 * M); ws[NF - 2 * M:] = (0.5 - 0.5 * np.cos(np.pi * j / M)) / wa[NF - 2 * M:]
    ring = np.zeros((K, NF)); acc = np.zeros((K, N + NF))
    spkp = np.zeros((K, L + N)); mic = np.zeros((K, N))
    h1 = torch.zeros(1, K, net.gru1.hidden_size); h2 = torch.zeros(1, K, net.gru2.hidden_size)
    Hrev = H[:, ::-1].copy()

    for s0 in range(0, N, HOP):
        e = s0 + HOP
        # leak[k, i] = sum_j H[k, j] * spk[k, s0+i-j]  (spk index in padded buffer: L + s0 + i - j)
        win = np.lib.stride_tricks.sliding_window_view(spkp[:, s0 + 1: L + e], L, axis=1)  # (K, HOP, L)
        leak = np.einsum("kil,kl->ki", win, Hrev)
        m = np.clip(src[:, s0:e] + leak * gain[:, s0:e], -1, 1)
        mic[:, s0:e] = m

        # network: one frame per hop over the latest NF samples (same alignment as the plugin)
        ring = np.concatenate([ring[:, HOP:], m], axis=1)
        X = np.fft.rfft(ring * wa, axis=1)
        lp = torch.tensor(np.log10(np.abs(X) ** 2 + 1e-10), dtype=torch.float32)[:, None]
        x = torch.relu(net.inp((lp - net.mean) / net.std))
        y1, h1 = net.gru1(x, h1); y2, h2 = net.gru2(y1, h2)
        mk = torch.sigmoid(net.out(torch.cat([y1, y2], -1)))[:, 0].numpy()
        y = np.fft.irfft(X * mk, NF, axis=1)
        # last 2M synthesized samples -> output times e .. e+2M (latency 2M)
        acc[:, e: e + 2 * M] += y[:, NF - 2 * M:] * ws[NF - 2 * M:]
        spkp[:, L + s0: L + e] = np.tanh(acc[:, s0:e] * drive) / drive
    return mic, np.stack([s[1] for s in sc]), np.array([s[5] for s in sc])


def shard(args):
    idx, batches, ckpt = args
    path = f"{ROOT}/data/sets/closed_{TAG}{idx:03d}.npz"
    if os.path.exists(path): return path
    torch.set_num_threads(1)
    net = T.Net(); net.load_state_dict(torch.load(ckpt)); net.eval()
    files = [f for f in sorted(glob.glob(f"{ROOT}/data/voice/*.npy")) if "p107" not in f]
    voices = [np.load(f, mmap_mode="r") for f in files]
    rng = np.random.default_rng(abs(hash((TAG, idx))) % (2 ** 32))
    mics, tgts, gs = [], [], []
    for _ in range(batches):
        a, b, g = run_batch(rng, voices, net)
        mics.append(a.astype(np.float16)); tgts.append(b.astype(np.float16)); gs.append(g)
    np.savez(path, mic=np.concatenate(mics), tgt=np.concatenate(tgts), g=np.concatenate(gs))
    return path


if __name__ == "__main__":
    ckpt, shards, batches = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    with Pool(4) as p:
        for r in p.imap_unordered(shard, [(i, batches, ckpt) for i in range(shards)]):
            print(r, flush=True)
    print("CLOSED_DONE", flush=True)
