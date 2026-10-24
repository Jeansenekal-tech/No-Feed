import glob, os, sys, numpy as np
from multiprocessing import Pool

ROOT = os.environ.get("NOFEED_AI_DIR", os.path.expanduser("~/nofeed-ai"))  # data + checkpoints
sys.path.insert(0, os.path.dirname(__file__)); import sim
N = 3 * 48000
OUT = f"{ROOT}/data/sets"

def shard(args):
    split, idx, count = args
    path = f"{OUT}/{split}_{idx:03d}.npz"
    if os.path.exists(path): return path
    files = sorted(glob.glob(f"{ROOT}/data/voice/*.npy"))
    files = [f for f in files if ("p107" in f) == (split == "val")]
    voices = [np.load(f, mmap_mode="r") for f in files]
    rng = np.random.default_rng(1000 * (split == "val") + idx)
    mic = np.zeros((count, N), np.float16); tgt = np.zeros((count, N), np.float16); g = np.zeros(count, np.float32)
    for i in range(count):
        m, t, gg = sim.make_example(rng, voices, N)
        mic[i], tgt[i], g[i] = m, t, gg
    np.savez(path, mic=mic, tgt=tgt, g=g)
    return path

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    jobs = [("train", i, 250) for i in range(28)] + [("val", i, 150) for i in range(2)]
    with Pool(4) as p:
        for r in p.imap_unordered(shard, jobs): print(r, flush=True)
    print("GEN_DONE")
