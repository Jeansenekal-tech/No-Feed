# NoFeed

**AI feedback suppressor** for live vocals, in the spirit of Alpha Labs De-Feedback. A neural network
trained on thousands of simulated mic→speaker→room feedback loops keeps the **voice** and removes
everything else (feedback, room echo and noise) before it can build up into a howl. It comes as a
**VST3** plugin (Windows, macOS, Linux), an **Audio Unit** (macOS), and a **standalone app**.

![NoFeed UI](docs/screenshot.png)

## Install

Download the installer for your system from the
[**latest release**](https://github.com/Jeansenekal-tech/no-feed/releases/latest):

| System | File | What to do |
|---|---|---|
| **Windows** | `NoFeed-x.y.z-Windows-Setup.exe` | Run it and follow the wizard. It installs the VST3 to `C:\Program Files\Common Files\VST3` and adds the standalone app to the Start menu. You can uninstall it from *Settings → Apps*. |
| **macOS** (Apple Silicon and Intel) | `NoFeed-x.y.z-macOS.pkg` | **Right-click → Open** the first time (see the note below). It installs the VST3, the AU and `NoFeed.app`, and you can untick any of them under *Customize*. |
| **Linux** | `NoFeed-x.y.z-Linux-x86_64.tar.gz` | Extract it and run `./install.sh` (current user) or `sudo ./install.sh --system` (all users). Run `./install.sh --uninstall` to remove it. |

After installing, **rescan plugins** in your DAW and look for **NoFeed** (by UniqueAV) under *Restoration*.

> **Unsigned installers:** the installers aren't signed with a paid Microsoft or Apple
> developer certificate, so you'll see one warning the first time you run them.
> On Windows, SmartScreen shows *"Windows protected your PC"*: click **More info → Run anyway**.
> On macOS, Gatekeeper says the package "can't be opened": **right-click → Open → Open**, or go to
> *System Settings → Privacy & Security → Open Anyway*.

## Troubleshooting

**Plugin is "blacklisted" or missing in Waves SuperRack Performer (or another host)**
1. Install the latest NoFeed release. Version 1.0.1 has no dependency on the Microsoft VC++
   runtime and supports mono, stereo and mono-to-stereo racks.
2. Make sure the plugin is in the default VST3 folder (`C:\Program Files\Common Files\VST3` on
   Windows, `/Library/Audio/Plug-Ins/VST3` on macOS). If you use a custom folder, set it in
   SuperRack under *Set Plugins Folder → Custom Folder*.
3. In SuperRack Performer, **clear the VST3 cache and rescan**. A plugin that was blacklisted
   once stays blacklisted until the cache is cleared, even after you update it.
4. The plugin list only shows plugins that match the rack's channel format, so check the rack type too.

## What it does

The chain is **AI voice isolation → Notch Assist (optional) → De-Ess → Output**.

- **AI voice isolation** (5.3 ms latency, reported to the DAW): a recurrent neural network looks at
  the spectrum every 2.7 ms and works out which parts are the dry voice. Everything else gets
  removed, including feedback, the speaker's own sound coming back into the mic, room reverb, hiss
  and hum. Removing the speaker's sound breaks the feedback loop itself, so feedback can't build up.
  This is the same idea as Alpha Labs De-Feedback.
- **Notch Assist** (on by default, zero latency): classic adaptive notch filters that hunt for
  ringing tones. They act as a safety net behind the AI. AI and notches together were the
  strongest combination in every test.
- **De-Ess** (zero latency): dips the top end only while an "s" or "sh" is happening.

### Controls

| | |
|---|---|
| **Amount** | How much of the non-voice signal the AI removes (100 % = everything it detects). |
| **De-Ess / Ess Freq** | De-esser amount (0 % = off) and the sibilance frequency. |
| **Output** | Output level. |
| **Listen** | Hear only what is being removed. |
| **Notch Assist** | The classic notch filters behind the AI (on by default), with their own Sensitivity, Speed, Release, Max Depth, Notch Q, Filters, Low/High Limit, Lock and Reset controls. |

The display shows the input (grey) and the voice the AI keeps (cyan). The gap between them is
what the AI removes, and the readout under **Amount** shows how many dB it is currently taking out.

**Tips:** put NoFeed first on the vocal mic channel and leave Amount at 100 %. If the voice sounds
too dry or processed for a quiet room, back Amount off to 60–80 %.

### How well it works

Benchmark on 96 simulated rooms and a speaker that the model never saw during training
(`training/eval_closed.py`). The table shows the share of rooms where the loop still feeds back:

| Loop gain past the feedback point | No processing | NoFeed AI |
|---|---|---|
| +0 to +3 dB | 23 % | **2 %** |
| +3 to +6 dB | 80 % | **3 %** |
| +6 to +8 dB | 95 % | **14 %** |

In other words, NoFeed gives you several dB of extra **gain before feedback**. A growing howl is
cut by about 20 dB, and a steady tone by about 12 dB.

**Known limits:** the AI was trained on speech and some singing at 48 kHz (44.1–192 kHz hosts are
supported). It is tuned for vocals, not instruments. It needs roughly 10–15 % of one CPU core per
instance.

## Building from source

You need CMake ≥ 3.22 and a C++17 compiler (Visual Studio 2022, Xcode, or GCC/Clang). JUCE 8 is
downloaded automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The output lands in `build/NoFeed_artefacts/Release/`. On Linux, install these first:
`sudo apt install libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev libxext-dev libfreetype6-dev libfontconfig1-dev`

Tests: `NoFeedAiTest` checks that the C++ network matches PyTorch, that the low-latency STFT
reconstructs perfectly, and the CPU cost. `NoFeedTests` covers Notch Assist and the de-esser.
The training pipeline lives in [`training/`](training/README.md).

## Making a release

1. Update the version number in `VERSION` (for example `1.0.1`).
2. Commit and push to `main`.

GitHub Actions then builds the plugin on Windows, macOS and Linux and runs the tests. It builds
each installer, **installs it on a clean machine** to check it, and publishes all three as the
`v1.0.1` release. The release only goes public once every installer has passed. Pushes that don't
change `VERSION` just build and test.

## License note

NoFeed is built with [JUCE](https://juce.com). JUCE 8 is available under the AGPLv3, or under a
JUCE licence (there is a free *Personal* tier for small revenue). Pick a licence for this
repository that matches how you plan to distribute it.

The AI model is trained on the [EARS dataset](https://github.com/facebookresearch/ears_dataset),
which is licensed **CC BY-NC 4.0 (non-commercial)**. You can use and share NoFeed for free, but
selling it requires retraining on commercially licensed speech (see `training/README.md`).
