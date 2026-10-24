"""Closed-loop benchmark on rooms and a speaker the model never saw.

    python eval_closed.py bypass ~/nofeed-ai/model/best.pt [more checkpoints...]

For each model, 100 random realistic rooms (same rooms for every model) are simulated with the
network inside the feedback loop and the held-out speaker p107 talking. A loop counts as
"feeding back" if, in the last second, the mic level is more than 6 dB above the dry voice.
`bypass` runs the same rooms with no processing.
"""
import os, sys
import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import closedloop_fast as C, train as T

ROOT = os.environ.get("NOFEED_AI_DIR", os.path.expanduser("~/nofeed-ai"))
torch.set_num_threads(4)
voices = [np.load(f"{ROOT}/data/voice/p107.npy", mmap_mode="r")]

for name in sys.argv[1:]:
    net = T.Net()
    if name == "bypass":
        with torch.no_grad():          # mask = 1 everywhere
            net.out.weight.zero_(); net.out.bias.fill_(30.0)
    else:
        net.load_state_dict(torch.load(name))
    net.eval()

    rng = np.random.default_rng(777)
    excess, gains = [], []
    for _ in range(4):
        mic, tgt, g = C.run_batch(rng, voices, net)
        late = slice(2 * 48000, 3 * 48000)
        v = tgt[:, late].std(1)
        ok = v > 1e-4                  # skip scenes where the talker is silent at the end
        excess += list(20 * np.log10(mic[ok][:, late].std(1) / v[ok])); gains += list(g[ok])
    excess, gains = np.array(excess), np.array(gains)
    for lo, hi in [(0, 3), (3, 6), (6, 8)]:
        sel = (gains >= lo) & (gains < hi)
        print(f"{os.path.basename(name):16s} +{lo}..+{hi} dB past feedback point: "
              f"feeding back in {np.mean(excess[sel] > 6) * 100:3.0f}% of {sel.sum()} rooms "
              f"(median {np.median(excess[sel]):+5.1f} dB vs. dry voice)", flush=True)
