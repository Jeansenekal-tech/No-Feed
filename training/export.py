"""Pack a trained checkpoint into the plugin's model format (NFM1) plus a parity test file."""
import os, struct, sys, numpy as np, torch
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import train as T

ROOT = os.environ.get("NOFEED_AI_DIR", os.path.expanduser("~/nofeed-ai"))  # data + checkpoints
ckpt, out_bin, parity_bin = sys.argv[1], sys.argv[2], sys.argv[3]
net = T.Net(); net.load_state_dict(torch.load(ckpt)); net.eval()
H = net.inp.out_features
sd = {k: v.detach().numpy().astype(np.float32) for k, v in net.state_dict().items()}

with open(out_bin, "wb") as f:
    f.write(b"NFM1"); f.write(struct.pack("<ii", T.BINS, H))
    for k in ["mean", "std", "inp.weight", "inp.bias",
              "gru1.weight_ih_l0", "gru1.weight_hh_l0", "gru1.bias_ih_l0", "gru1.bias_hh_l0",
              "gru2.weight_ih_l0", "gru2.weight_hh_l0", "gru2.bias_ih_l0", "gru2.bias_hh_l0",
              "out.weight", "out.bias"]:
        f.write(np.ascontiguousarray(sd[k]).tobytes())

# parity: features of a real validation clip and the masks PyTorch computes for them

d = np.load(f"{ROOT}/data/sets/val_000.npz" if os.path.exists(f"{ROOT}/data/sets/val_000.npz") else f"{ROOT}/data/sets/train_000.npz")
x = torch.tensor(d["mic"][3:4, :48000].astype(np.float32))
with torch.no_grad():
    lp = T.logpow(T.stft(x))[:, :60]
    m = net(lp)
with open(parity_bin, "wb") as f:
    f.write(struct.pack("<ii", lp.shape[1], T.BINS))
    f.write(lp[0].numpy().astype(np.float32).tobytes()); f.write(m[0].numpy().astype(np.float32).tobytes())
print("exported", out_bin, "hidden", H, "params", sum(p.numel() for p in net.parameters()))
