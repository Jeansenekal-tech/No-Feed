"""Train the NoFeed voice-isolation network.

Input : log-power STFT of the mic signal (asymmetric low-latency window, 1024/128).
Output: a per-bin gain mask (513 bins) that keeps the dry voice and removes feedback,
        the loop's re-emission of the voice, room reverb and noise.
"""
import glob, math, os, sys, time
import numpy as np
import torch
import torch.nn as nn

ROOT = os.environ.get("NOFEED_AI_DIR", os.path.expanduser("~/nofeed-ai"))  # data + checkpoints

N_FFT, HOP, M = 1024, 128, 128
BINS = N_FFT // 2 + 1
torch.set_num_threads(4)


def analysis_window():
    n = np.arange(N_FFT, dtype=np.float64)
    w = np.empty(N_FFT)
    rise = n < N_FFT - M
    w[rise] = np.sqrt(0.5 - 0.5 * np.cos(np.pi * n[rise] / (N_FFT - M)))
    k = n[~rise] - (N_FFT - M)
    w[~rise] = np.sqrt(0.5 + 0.5 * np.cos(np.pi * k / M))
    return w


class Net(nn.Module):
    def __init__(self, hidden=256):
        super().__init__()
        self.register_buffer("mean", torch.zeros(BINS))
        self.register_buffer("std", torch.ones(BINS))
        self.inp = nn.Linear(BINS, hidden)
        self.gru1 = nn.GRU(hidden, hidden, batch_first=True)
        self.gru2 = nn.GRU(hidden, hidden, batch_first=True)
        self.out = nn.Linear(2 * hidden, BINS)

    def forward(self, logp):
        x = torch.relu(self.inp((logp - self.mean) / self.std))
        y1, _ = self.gru1(x)
        y2, _ = self.gru2(y1)
        return torch.sigmoid(self.out(torch.cat([y1, y2], -1)))


WIN = torch.tensor(analysis_window(), dtype=torch.float32)


def stft(x):
    X = torch.stft(x, N_FFT, HOP, N_FFT, WIN, center=False, return_complex=True)
    return X.transpose(1, 2)  # (B, T, F)


def logpow(X):
    return torch.log10(X.real ** 2 + X.imag ** 2 + 1e-10)


def loss_fn(mask, X, S):
    A, T = X.abs(), S.abs()
    est = mask * A
    ce, ct = (est + 1e-8) ** 0.3, (T + 1e-8) ** 0.3
    w = torch.where(ce < ct, 2.0, 1.0)            # removing voice is worse than leaving residue
    return (w * (ce - ct) ** 2).mean()


def load(split):
    mics, tgts = [], []
    for f in sorted(glob.glob(f"{ROOT}/data/sets/{split}_*.npz")):
        d = np.load(f)
        mics.append(d["mic"]); tgts.append(d["tgt"])
    return np.concatenate(mics), np.concatenate(tgts)


def main():
    steps = int(sys.argv[1]) if len(sys.argv) > 1 else 6000
    out_dir = f"{ROOT}/model"; os.makedirs(out_dir, exist_ok=True)
    mic, tgt = load("train"); vmic, vtgt = load("val")
    print("train", mic.shape, "val", vmic.shape, flush=True)

    net = Net()
    # input normalisation statistics
    with torch.no_grad():
        idx = np.random.default_rng(0).choice(len(mic), 200, replace=False)
        lp = logpow(stft(torch.tensor(mic[idx].astype(np.float32))))
        net.mean.copy_(lp.mean((0, 1))); net.std.copy_(lp.std((0, 1)) + 1e-3)

    opt = torch.optim.AdamW(net.parameters(), lr=1e-3, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=1.5e-3, total_steps=steps, pct_start=0.05)
    rng = np.random.default_rng(1)
    B, SEG = 16, 2 * 48000
    best = float("inf"); t0 = time.time()

    def val_loss():
        net.eval(); tot = 0
        with torch.no_grad():
            for i in range(0, len(vmic), 32):
                X = stft(torch.tensor(vmic[i:i + 32].astype(np.float32)))
                S = stft(torch.tensor(vtgt[i:i + 32].astype(np.float32)))
                tot += loss_fn(net(logpow(X)), X, S).item() * len(X)
        net.train(); return tot / len(vmic)

    for step in range(1, steps + 1):
        idx = rng.integers(0, len(mic), B)
        off = rng.integers(0, mic.shape[1] - SEG, B)
        xb = np.stack([mic[i, o:o + SEG] for i, o in zip(idx, off)]).astype(np.float32)
        tb = np.stack([tgt[i, o:o + SEG] for i, o in zip(idx, off)]).astype(np.float32)
        g = 10 ** (rng.uniform(-6, 6, (B, 1)) / 20).astype(np.float32)   # level augmentation
        X, S = stft(torch.tensor(np.clip(xb * g, -1, 1))), stft(torch.tensor(tb * g))
        loss = loss_fn(net(logpow(X)), X, S)
        opt.zero_grad(); loss.backward()
        nn.utils.clip_grad_norm_(net.parameters(), 1.0)
        opt.step(); sched.step()

        if step % 100 == 0:
            print(f"step {step} loss {loss.item():.4f} {(time.time() - t0) / step:.2f}s/step", flush=True)
        if step % 500 == 0 or step == steps:
            vl = val_loss()
            print(f"  val {vl:.4f}", flush=True)
            if vl < best:
                best = vl; torch.save(net.state_dict(), f"{out_dir}/best.pt")
    print("TRAIN_DONE best val", best, flush=True)


if __name__ == "__main__":
    main()
