# GABBERKIX

**A hardcore kick designer for the M-VAVE FM-1.** A kick in three layers: a midbody for character, a punch for the
attack and a sub for the low end, pressed into one kick by groups driven hard into clippers.

> **Status: phase 2 of 6, not yet tried on an FM-1.** The midbody is in: a synthesized 909 through a lowpass with
> drive, an eight-band parametric EQ, a distortion and an EQ after it, played on every beat or pitched from the
> keys and MIDI. Punch, groups, sub and presets follow (`kick-firmware-instructions.md`).

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) and felucca-bsp, from Felucca by Leo Kuroshita
(Hügelton Instruments).

## The sound so far

```
909 + DRIVE -> EQ (8 peaks) -> LPF -> DIST -> POST (2 peaks)
```

The source is a blank 909: a sine whose pitch falls from a sweep to the root (G1, 49 Hz), a short noise click on the
attack, and a hold-then-decay envelope. Everything after it is there to choose what the distortion gets: extreme EQ
boosts and cuts in front of a clipper make the kick's character; the distortion alone
gives an oldschool gabber kick.

- **DRIVE** (on the 909 page): a soft saturation of the 909 before the EQ. The 909 alone is close to a pure sine,
  with next to nothing above 300 Hz for the EQ to boost; the drive makes the overtones the EQ shapes. 0 dB: off.
- **EQ**: a parametric EQ of eight peaks, each up to ±30 dB and Q 30: wide boosts and deep, narrow cuts. A new kick
  has them at 0 dB (40 Hz to 5 kHz): they do nothing until their gain moves. There is no low cut on purpose: the lows
  are what a kick is, and what drives the distortion; a low band turned down takes some away.
- **LPF**: a lowpass with resonance, just before the distortion, to tame what the EQ boosted. Open and with no
  resonance (a new kick's), it does nothing.
- **DIST**: TANH (soft), HARD (a hard clip) or GABBER (a soft stage 12 dB hotter with a buzz
  of even harmonics, into a hard clip); drive up to 60 dB, a bias for asymmetry, out level and dry/wet mix.
- **POST**: two more peaks, as the EQ's, after the distortion.
- **The master**: a peak limiter at -0.3 dBFS, then MASTER's volume.

A new kick has the distortion on, GABBER at 18 dB: a classic gabber kick at once. Its OUT is at -9 dB, which keeps it
about as loud as the clean 909: a distorted kick is a near-square wave, far denser than a clean one. Turn OUT up to
push the limiter, down when the drive goes up.

Everything is computed in single-precision float, which the FM-1's FPU runs; inside the engine the signal may go far
above full scale, and only the master stops it. Every filter is one state-variable filter, and the curve on
the screen is computed from the same coefficients the sound uses (`tools/chain_test.c` checks they agree).

## Controls

| Control | What it does |
| --- | --- |
| **SELECT** | The page: 909, EQ, LPF, DIST, POST, and round again; in the order of the sound, so walking through them builds the kick |
| **HOME** | The 909 page |
| **EDIT** | The EQ page; again: the next band |
| **LFO** | The LPF page |
| **FX** | The DIST page |
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
| **909** | SWEEP (how far above the root the pitch starts, in octaves), SW.T (how fast it falls), HOLD, DECAY (to -60 dB) | TUNE (the root, ±12 semitones from G1), CLICK, LEVEL, DRIVE |
| **EQ** | BAND (1–8), and its GAIN, Q, FREQ | |
| **LPF** | CUTOFF, RESO | |
| **DIST** | D.TYPE, D.DRV, BIAS, OUT | MIX |
| **POST** | BAND (A, B), and its GAIN, Q, FREQ | |

Along the top, the pages as tabs, the one shown lit; each page has its own colour (909 red, EQ cyan, LPF amber, DIST
magenta, POST green), so a glance tells where you are. Under the knobs the screen shows the kick's first second (909); the filters before the distortion as an EQ plugin
draws them, each band a numbered dot with its own faint curve, the band the knobs turn lit, their sum the bright line
(EQ, LPF; on LPF a line at the cutoff); the kick's spectrum (DIST); or the spectrum with the POST's
bands over it (POST).

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
build/kick_render kick.wav GAIN3=18 Q3=400 DRIVE=12 CUTOFF=900 RESO=60 D.TYPE=GABBER D.DRV=30
                                              # drive, a band boosted, a resonant lowpass, GABBER
```

Install over USB from Windows (Python with mido and python-rtmidi):

```
python base/tools/fm1_install.py app/build/gabberkix.fwsc
```

## Licence

GPL-3.0-only (LICENSE). fm1-base's files keep their own licence, MPL-2.0.
