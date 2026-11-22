# GABBERKIX

**A hardcore kick designer for the M-VAVE FM-1.** A kick in three layers: a midbody for character, a punch for the
attack and a sub for the low end, pressed into one kick by groups driven hard into clippers.

> **Status: phase 3 of 6.** The midbody (a synthesized 909 through two rounds of EQ and distortion) and the punch
> (the hard tick at the start of a kick) are in, pressed together in group A's clipper; played on every beat or
> pitched from the keys and MIDI, with sixteen presets and a randomizer. The sub follows
> (`kick-firmware-instructions.md`).

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) and felucca-bsp, from Felucca by Leo Kuroshita
(Hügelton Instruments).

## The sound so far

```
midbody: 909 -> EQ (4 peaks) -> DIST1 -> EQ2 (4 peaks) -> DIST2 -> POST (2 peaks) --+
punch:   a kick (this one, or a preset) -> GATE -> P.EQ (4 peaks) -> DRIVE -> LEVEL -+-> GROUP A: EQ -> CLIP -> OUT
                                                                                      -> limiter
```

The source is a blank 909: a sine whose pitch falls from a sweep to the root (G1, 49 Hz), a short noise click on the
attack, and a hold-then-decay envelope. The character comes from extreme EQ in front of a distortion: boosts and cuts
choose what the distortion does, the distortion makes overtones, and the next EQ finds the sweet spots among them
before the next distortion presses them together. EQs in a row are no more than one EQ; it is the distortions between
them that make the variety. Before a hard clipper the low bands (40-250 Hz) matter most; a milder first distortion
leaves the EQs more to do.

- **EQ**: a parametric EQ of four peaks, each up to ±30 dB and Q 30: wide boosts and deep, narrow cuts. There is no
  low cut on purpose: the lows are what a kick is, and what drives the distortion; a low band turned down takes some
  away. A new kick has every band at 0 dB, where it does nothing.
- **DIST1**, **DIST2**: TANH (soft), HARD (a hard clip) or GABBER (a soft stage 12 dB hotter with a buzz of even
  harmonics, into a hard clip); drive up to 60 dB, a bias for asymmetry, out level and dry/wet mix.
- **EQ2**: four more peaks, the same, between the distortions.
- **POST**: two more peaks, after them.
- **PNCH**, the punch: the hard tick at the start of a kick, as a finished kick's first milliseconds. SRC is where it
  comes from: SELF (this kick) or a preset (P1–P16, with its own EQs and distortions: "a kick I made earlier"), or
  OFF. P.GAIN sets how loud the tick is (-36 to +24 dB), the gate (P.HOLD, then P.DECAY to -60 dB in 1–200 ms) how
  much of the kick's start it keeps, P.DRV clips it (allowed, and meant), P.TUNE plays the source higher or lower (a
  sharper or duller tick).
- **P.EQ**: four peaks on the punch, to make it fit the kick.
- **GRP.A**, group A: the midbody and the punch summed, an EQ of four peaks, then a brickwall clipper: PUSH drives the
  sum into it (HARD, or SOFT: straight to half scale, then bent), OUT after it. At no push it does next to nothing; far
  over full scale it presses the two layers together, so they sound as one kick, not two.
- **The master**: a peak limiter at -0.3 dBFS, then MASTER's volume.

**The dice** (ARP) draw an extreme curve on both EQs, as a kick's EQ is drawn: cuts deep (15–30 dB) and narrow,
boosts wide (9–24 dB); EQ's bands between 30 Hz and 1 kHz, where they steer the first distortion, but no cut at the
root (35–90 Hz: the kick's body stays), EQ2's between 100 Hz and 8 kHz, with no boost above 5 kHz and none over 12 dB
above 2 kHz (not shrill). Each distortion gets a type, a drive (DIST1 6–30 dB, DIST2 milder, 0–15 dB) and now and then a bias. The
909 gets a SWEEP of 1–5 octaves, a CLICK of 0–80 % and a DECAY of 200–900 ms, every length as likely (its TUNE, SW.T,
HOLD and LEVEL stay, and so does POST). The punch is off one time in two (towards the oldschool gabber kick); else it gets a P.GAIN of -12 to +6 dB, a P.HOLD of 0–10 ms, a P.DECAY of 1–200
ms (every length as likely), a P.TUNE of ±12 and its own curve on P.EQ (its SRC and P.DRV stay, and so does group A). Then the kick is rendered once,
measured after the limiter, and DIST2's OUT set so it is as loud as a new kick: never soft, never silent. SEL + ARP
takes the last throw back.

A new kick is a classic gabber kick at once: DIST1 a soft TANH at 12 dB, DIST2 a GABBER at 6 dB with its OUT at
-9 dB, its own tick on top (SELF, 8 ms, +6 dB), group A pushed 6 dB into a hard clip, its OUT at -6 dB: about as loud
as the clean 909 (a distorted kick is a near-square wave, far denser than a clean one). Turn an
OUT up to push the next stage or the limiter, down when its drive goes up.

Everything is computed in single-precision float, which the FM-1's FPU runs; inside the engine the signal may go far
above full scale, and only the master stops it. Every filter is one state-variable filter, and the curves on the
screen are computed from the same coefficients the sound uses (`tools/chain_test.c` checks they agree).

## Controls

| Control | What it does |
| --- | --- |
| **SELECT** | The page: 909, EQ, D1, EQ2, D2, POST, PNCH, P.EQ, GRP.A, and round again; in the order of the sound, so walking through them builds the kick |
| **HOME** | The 909 page |
| **EDIT** | The EQ page; again: the next band |
| **FX** | D1; again: D2, and back |
| **ENV** | PNCH; again: P.EQ, and back |
| **SEQ** | GRP.A; again: its next band |
| **LFO** | The EQ2 page; again: the next band |
| **GLO** | The POST page; again: the other band |
| **SEL** held + white key | Load the preset on that key |
| **SAVE** held + white key | Save the kick on that key (16 presets, one a white key) |
| **ARP** | Throw the dice: the 909's SWEEP, CLICK and DECAY, extreme curves on both EQs and new distortions, at the loudness of a new kick |
| **SEL** held + **ARP** | Undo the last throw |
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
| **EQ**, **EQ2**, **POST**, **P.EQ** | BAND (1–4, 1–4, A–B, 1–4), and its GAIN, Q, FREQ | |
| **D1**, **D2** | TYPE, DRIVE, BIAS, OUT | MIX |
| **PNCH** | SRC, P.GAIN, P.HOLD, P.DECAY | P.DRV, P.TUNE |
| **GRP.A** | BAND (1–4), and its GAIN, Q, FREQ | PUSH, CLIP, A.OUT |

Along the top, the pages as tabs, the one shown lit; each page has its own colour (909 red, EQ cyan, D1 magenta,
EQ2 blue, D2 amber, POST green, PNCH yellow, P.EQ teal, GRP.A pink), so a glance tells where you are. Under the knobs the screen shows the kick's first
second (909), zoomed so its body fills the height (the zoom at the right, X2, X3: the tick at the start may run into
the edge); on an EQ page its bands as an EQ plugin draws them (each a numbered dot with its own faint curve, the
band the knobs turn lit, their sum the bright line) over the spectrum after the next distortion, where its sweet
spots show (on P.EQ the punch's, on GRP.A the spectrum after the clipper); on a distortion page the spectrum after it;
on PNCH the first 50 ms, the whole kick faint and the punch bright over it.

## Presets

Sixteen kicks, one on each white key. Hold **SAVE** and press a white key to save the kick there; hold **SEL** and
press one to load it. While SEL or SAVE is held, the white keys that hold a preset light dim, and the one loaded or
saved last lights bright. The header shows it (P3), with a star once the kick has changed since. The presets are kept
in the FM-1's flash, outside the firmware: they survive a restart and an update. A preset is the parameters alone (no
audio); parameters are only ever added at the end of the list, so a preset saved now still loads in a later version.
A preset saved before the punch loads with no punch and group A a straight wire: it sounds as it did.

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
