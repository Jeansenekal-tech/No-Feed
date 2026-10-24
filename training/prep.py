# Convert EARS speakers to one int16 array per speaker (active speech only), deleting the wavs.
import glob, os, sys, numpy as np, soundfile as sf

ROOT = os.environ.get("NOFEED_AI_DIR", os.path.expanduser("~/nofeed-ai"))  # data + checkpoints

root = f"{ROOT}/data"
os.makedirs(f"{root}/voice", exist_ok=True)
for spk in sorted(glob.glob(f"{root}/ears/p*")):
    name = os.path.basename(spk)
    out = f"{root}/voice/{name}.npy"
    if os.path.exists(out) or not os.path.isdir(spk):
        continue
    chunks = []
    for f in sorted(glob.glob(f"{spk}/*.wav")):
        x, sr = sf.read(f, dtype="float32")
        assert sr == 48000
        if x.ndim > 1: x = x[:, 0]
        # drop long silences: keep 20 ms frames within 45 dB of the file's loudest frame
        n = len(x) // 960 * 960
        fr = x[:n].reshape(-1, 960)
        e = 10 * np.log10((fr ** 2).mean(1) + 1e-12)
        keep = e > e.max() - 45
        keep = np.convolve(keep, np.ones(5), "same") > 0          # keep a little context
        y = fr[keep].reshape(-1)
        y = y / (np.abs(y).max() + 1e-9) * 0.9
        chunks.append((y * 32767).astype(np.int16))
    np.save(out, np.concatenate(chunks))
    print(name, sum(len(c) for c in chunks) / 48000 / 60, "min", flush=True)
    for f in glob.glob(f"{spk}/*.wav"): os.remove(f)
    os.rmdir(spk)
