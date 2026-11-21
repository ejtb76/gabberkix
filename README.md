# GABBERKIX

**A hardcore kick designer for the M-VAVE FM-1.** A kick in three layers: a midbody for character, a punch for the
attack and a sub for the low end, pressed into one kick by groups driven hard into clippers.

> **Status: phase 2 of 6, not yet tried on an FM-1.** The midbody is in: a synthesized 909 through a filter with
> drive, an eight-band parametric EQ, a resonant filter, a distortion and an EQ after it, played on every beat or pitched from the
> keys and MIDI. Punch, groups, sub and presets follow (`kick-firmware-instructions.md`).

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) and felucca-bsp, from Felucca by Leo Kuroshita
(Hügelton Instruments).

## The sound so far

```
909 -> PRE -> EQ (8 peaks) -> FILTER -> DIST -> POST (2 peaks)
```

The source is a blank 909: a sine whose pitch falls from a sweep to the root (G1, 49 Hz), a short noise click on the
attack, and a hold-then-decay envelope. Everything after it is there to choose what the distortion gets: extreme EQ
boosts and cuts and a screaming resonant filter in front of a clipper make the kick's character; the distortion alone
gives an oldschool gabber kick.

- **PRE**: a filter (LP, HP, BP, notch) with a drive in front of it.
- **EQ**: a parametric EQ of eight peaks, each up to ±30 dB and Q 30: wide boosts and deep, narrow cuts. A new kick
  has them at 0 dB (40 Hz to 5 kHz): they do nothing until their gain moves. There is no low cut on purpose: the lows
  are what a kick is, and what drives the distortion; a low band turned down takes some away.
- **FILTER**: the filter just before the distortion, the one that shapes the kick most: LP, HP, BP or notch, resonance
  up to Q 45, a drive, an envelope that starts the cutoff up to six octaves higher and lets it fall, and key tracking.
- **DIST**: TANH (soft), HARD (a hard clip), FOLD (a sine wavefolder) or GABBER (a soft stage 12 dB hotter with a buzz
  of even harmonics, into a hard clip); drive up to 60 dB, a bias for asymmetry, out level and dry/wet mix.
- **POST**: two more peaks, as the EQ's, after the distortion.

Everything is computed in single-precision float, which the FM-1's FPU runs; inside the engine the signal may go far
above full scale, and only the master stops it at 0 dBFS. Every filter is one state-variable filter, and the curve on
the screen is computed from the same coefficients the sound uses (`tools/chain_test.c` checks they agree).

## Controls

| Control | What it does |
| --- | --- |
| **SELECT** | The page: 909, PRE, EQ, FILTER, DIST, POST |
| **HOME** | The 909 page |
| **EDIT** | The EQ page; again: the next band |
| **LFO** | The FILTER page; again: PRE, and back |
| **FX** | The DIST page |
| **GLO** | The POST page |
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
| **PRE** | P.TYPE, P.FREQ, P.RESO, P.DRV | |
| **EQ** | BAND (1–8), and its FREQ, GAIN, Q | |
| **FILTER** | F.TYPE, CUTOFF, RESO, F.DRV | ENV (octaves), ENV.T (how fast it falls), KEYTRK |
| **DIST** | D.TYPE, D.DRV, BIAS, OUT | MIX |
| **POST** | band A: FREQ, GAIN, Q | band B |

Under the knobs the screen shows the kick's first second (909); the filters before the distortion as an EQ plugin
draws them, each band a numbered dot with its own faint curve, the band the knobs turn lit, their sum the bright line
(PRE, EQ, FILTER; on PRE and FILTER a line at the cutoff); the kick's spectrum (DIST); or the spectrum with the POST's
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
build/kick_render kick.wav GAIN3=18 Q3=400 F.TYPE=BP CUTOFF=900 RESO=60 D.TYPE=GABBER D.DRV=30
                                              # a band boosted, a resonant band-pass, GABBER
```

Install over USB from Windows (Python with mido and python-rtmidi):

```
python base/tools/fm1_install.py app/build/gabberkix.fwsc
```

## Licence

GPL-3.0-only (LICENSE). fm1-base's files keep their own licence, MPL-2.0.
