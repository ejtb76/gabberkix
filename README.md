# GABBERKIX

**A hardcore kick designer for the M-VAVE FM-1.** A kick in three layers: a midbody for character, a punch for the
attack and a sub for the low end, pressed into one kick by groups driven hard into clippers.

> **Status: phase 2 of 6, tried on an FM-1.** The midbody is in: a synthesized 909 through two rounds of EQ and
> distortion and an EQ after them, played on every beat or pitched from the keys and MIDI. Punch, groups, sub and
> presets follow (`kick-firmware-instructions.md`).

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) and felucca-bsp, from Felucca by Leo Kuroshita
(Hügelton Instruments).

## The sound so far

```
909 -> EQ (8 peaks) -> DIST1 -> EQ2 (4 peaks) -> DIST2 -> POST (2 peaks) -> limiter
```

The source is a blank 909: a sine whose pitch falls from a sweep to the root (G1, 49 Hz), a short noise click on the
attack, and a hold-then-decay envelope. The character comes from extreme EQ in front of a distortion: boosts and cuts
choose what the distortion does, the distortion makes overtones, and the next EQ finds the sweet spots among them
before the next distortion presses them together. EQs in a row are no more than one EQ; it is the distortions between
them that make the variety. Before a hard clipper the low bands (40-250 Hz) matter most; a milder first distortion
leaves the EQs more to do.

- **EQ**: a parametric EQ of eight peaks, each up to ±30 dB and Q 30: wide boosts and deep, narrow cuts. There is no
  low cut on purpose: the lows are what a kick is, and what drives the distortion; a low band turned down takes some
  away. A new kick has every band at 0 dB, where it does nothing.
- **DIST1**, **DIST2**: TANH (soft), HARD (a hard clip) or GABBER (a soft stage 12 dB hotter with a buzz of even
  harmonics, into a hard clip); drive up to 60 dB, a bias for asymmetry, out level and dry/wet mix.
- **EQ2**: four more peaks, between the distortions.
- **POST**: two more peaks, after them.
- **The master**: a peak limiter at -0.3 dBFS, then MASTER's volume.

A new kick is a classic gabber kick at once: DIST1 a soft TANH at 12 dB, DIST2 a GABBER at 6 dB with its OUT at
-9 dB, about as loud as the clean 909 (a distorted kick is a near-square wave, far denser than a clean one). Turn an
OUT up to push the next stage or the limiter, down when its drive goes up.

Everything is computed in single-precision float, which the FM-1's FPU runs; inside the engine the signal may go far
above full scale, and only the master stops it. Every filter is one state-variable filter, and the curves on the
screen are computed from the same coefficients the sound uses (`tools/chain_test.c` checks they agree).

## Controls

| Control | What it does |
| --- | --- |
| **SELECT** | The page: 909, EQ, DIST1, EQ2, DIST2, POST, and round again; in the order of the sound, so walking through them builds the kick |
| **HOME** | The 909 page |
| **EDIT** | The EQ page; again: the next band |
| **FX** | DIST1; again: DIST2, and back |
| **LFO** | The EQ2 page; again: the next band |
| **GLO** | The POST page; again: the other band |
| **K1–K4** | The page's top row |
| **SEL** held + **K1–K4** | The second row |
| **PRESETS** | Tempo, 60–300 BPM (160 at power-on) |
| **PLAY** | A kick on every beat, at the root; again to stop |
| **Keys** | A kick at the key's pitch: the middle G (G4) is the root, the low G an octave down, the top G an octave up |
| **MIDI in** | Note on (USB, TRS, Bluetooth): a kick at the note's pitch, note 67 (G4) the root, other octaves folded into an octave down or up |
| **MASTER** | Volume |

The pages' knobs:

| Page | K1–K4 | SEL held + K1–K4 |
| --- | --- | --- |
| **909** | SWEEP (how far above the root the pitch starts, in octaves), SW.T (how fast it falls), HOLD, DECAY (to -60 dB) | TUNE (the root, ±12 semitones from G1), CLICK, LEVEL |
| **EQ**, **EQ2**, **POST** | BAND (1–8, 1–4, A–B), and its GAIN, Q, FREQ | |
| **DIST1**, **DIST2** | TYPE, DRIVE, BIAS, OUT | MIX |

Along the top, the pages as tabs, the one shown lit; each page has its own colour (909 red, EQ cyan, DIST1 magenta,
EQ2 blue, DIST2 amber, POST green), so a glance tells where you are. Under the knobs the screen shows the kick's first
second (909); on an EQ page its bands as an EQ plugin draws them (each a numbered dot with its own faint curve, the
band the knobs turn lit, their sum the bright line) over the spectrum after the next distortion, where its sweet
spots show; on a distortion page the spectrum after it.

## Building and testing

In WSL or on Linux, with the JieLi toolchain in `~/.jieli/toolchain` (see fm1-base's README):

```
git submodule update --init --recursive
sh build.sh                     # app/build/gabberkix.fwsc
sh tests/run_tests.sh           # the offline render's checks, the chain's, and the host simulator's scripts
```

`tools/build_render.sh` builds `build/kick_render`, the offline render: one kick from the firmware's own engine to a
32-bit float WAV, with its length, peak and the pitch of its body:

```
build/kick_render kick.wav                    # the blank 909
build/kick_render kick.wav 12 DECAY=900       # an octave up, a longer decay
build/kick_render kick.wav GAIN3=18 Q3=400 DRIVE1=18 GAIN2.2=20 Q2.2=600 TYPE2=HARD
                                              # a band boosted, DIST1 pushed, EQ2's band 2 up, DIST2 HARD
```

Install over USB from Windows (Python with mido and python-rtmidi):

```
python base/tools/fm1_install.py app/build/gabberkix.fwsc
```

## Licence

GPL-3.0-only (LICENSE). fm1-base's files keep their own licence, MPL-2.0.
