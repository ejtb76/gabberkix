# GABBERKIX

**A hardcore kick designer for the M-VAVE FM-1**, built the way hardcore kicks are built: a midbody for character, a
punch for the attack and a sub for the low end, pressed into one kick by groups driven hard into clippers.

> **Status: phase 1 of 6, not yet tried on an FM-1.** The midbody's source is in: a synthesized 909, played on every
> beat or pitched from the keys and MIDI. Filters, distortion, punch, groups, sub and presets follow
> (`kick-firmware-instructions.md`).

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) and felucca-bsp, from Felucca by Leo Kuroshita
(Hügelton Instruments).

## The sound so far

A blank 909: a sine whose pitch falls from a sweep to the root (G1, 49 Hz), a short noise click on the attack, and a
hold-then-decay envelope. Everything is computed in single-precision float, which the FM-1's FPU runs; inside the
engine the signal may go far above full scale, and only the master stops it at 0 dBFS.

## Controls

| Control | What it does |
| --- | --- |
| **SELECT** | The page (one so far: **909**) |
| **K1–K4** | The page's top row: SWEEP (how far above the root the pitch starts, in octaves), SW.T (how fast it falls), HOLD, DECAY (to -60 dB) |
| **SEL** held + **K1–K4** | The second row: TUNE (the root, ±12 semitones from G1), CLICK, LEVEL |
| **PRESETS** | Tempo, 60–300 BPM (160 at power-on) |
| **PLAY** | A kick on every beat, at the root; again to stop |
| **Keys** | A kick at the key's pitch: the middle G (G4) is the root, the low G an octave down, the top G an octave up |
| **MIDI in** | Note on (USB, TRS, Bluetooth): a kick at the note's pitch, note 67 (G4) the root, other octaves folded into an octave down or up |
| **MASTER** | Volume |

The screen shows the page's parameters, the first second of the kick as it is now, its length, and the pitch of the
root and of the last kick played.

## Building and testing

In WSL or on Linux, with the JieLi toolchain in `~/.jieli/toolchain` (see fm1-base's README):

```
git submodule update --init --recursive
sh build.sh                     # app/build/gabberkix.fwsc
sh tests/run_tests.sh           # the offline render's pitch checks and the host simulator's scripts
```

`tools/build_render.sh` builds `build/kick_render`, the offline render: one kick from the firmware's own engine to a
32-bit float WAV, with its length, peak and the pitch of its body:

```
build/kick_render kick.wav                    # the blank 909
build/kick_render kick.wav 12 DECAY=900       # an octave up, a longer decay
```

Install over USB from Windows (Python with mido and python-rtmidi):

```
python base/tools/fm1_install.py app/build/gabberkix.fwsc
```

## Licence

GPL-3.0-only (LICENSE). fm1-base's files keep their own licence, MPL-2.0.
