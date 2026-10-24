# Training the NoFeed AI model

The plugin embeds a small recurrent network (`Resources/model.bin`, ~1.2 M parameters) that
predicts a per-frequency "voice" mask. Everything that isn't the dry voice gets removed: feedback,
the speaker's re-emission of the voice, room reverb and noise. The plugin runs it in
`Source/AiVoiceEngine.cpp`.

## Pipeline

All data and checkpoints go to `$NOFEED_AI_DIR` (default `~/nofeed-ai`).

```bash
python -m venv venv && venv/bin/pip install torch numpy scipy soundfile
mkdir -p ~/nofeed-ai/data/ears && ./fetch.sh   # EARS speech dataset (48 kHz), ~8 GB
venv/bin/python prep.py                       # -> one int16 array per speaker
venv/bin/python gen.py                        # simulate 7300 closed feedback loops (3 s each)
venv/bin/python train.py 6000                 # round 0: ~2 h on a 4-core CPU, much faster on a GPU
cp ~/nofeed-ai/model/best.pt ~/nofeed-ai/model/round0.pt

# closed-loop rounds: run the current model *inside* simulated feedback loops, record what it
# lets through, and fine-tune on that (repeat 2-3 times)
NOFEED_ROUND=r1_ venv/bin/python closedloop_fast.py ~/nofeed-ai/model/round0.pt 4 20
venv/bin/python train.py 2500 ~/nofeed-ai/model/round0.pt 0.6
cp ~/nofeed-ai/model/best.pt ~/nofeed-ai/model/round1.pt   # ...and so on

venv/bin/python eval_closed.py bypass ~/nofeed-ai/model/round0.pt ~/nofeed-ai/model/round1.pt
venv/bin/python export.py ~/nofeed-ai/model/best.pt ../Resources/model.bin ../Tests/data/parity.bin
```

- `sim.py`: the feedback-loop simulator. It models delay, random loudspeaker EQ, a random room
  (early reflections plus a reverberant tail), loop gains from "ringing" up to "howling", amp
  saturation, ADC clipping, noise and hum, and a fader being pushed up during a clip. The target
  is always the dry voice.
- `train.py`: an asymmetric low-latency STFT (1024-sample analysis / 256-sample synthesis window,
  128-sample hop, so 5.3 ms latency), `Linear → GRU → GRU → Linear` with a sigmoid mask, and a
  compressed-magnitude loss that penalises removing voice twice as much as leaving residue.
- `closedloop_fast.py`: closed-loop rounds. A model trained only on open-loop recordings sees
  feedback that built up *without* it. Once it sits in the loop it changes the loop, and the
  residue it leaves gets re-amplified on every round trip. These rounds run the network (streaming,
  identical to the plugin) inside 25 simulated loops at once and record the mic signal it really
  faces.
- `eval_closed.py`: the benchmark. It uses the same 100 unseen rooms and the held-out speaker,
  reports how often the loop still feeds back, and compares against no processing.
- `export.py`: packs the weights into the plugin's `NFM1` format, plus a parity file. The C++
  test (`NoFeedAiTest`) uses that file to check the plugin computes the same masks as PyTorch.

Speaker `p107` is held out for validation and for the closed-loop test.

## Data licence

[EARS](https://github.com/facebookresearch/ears_dataset) is released under **CC BY-NC 4.0**, and
the shipped model is trained on it, so treat the model as **non-commercial**. To sell NoFeed,
retrain on commercially licensed speech: point `prep.py` at your own 48 kHz recordings and rerun
the pipeline.
