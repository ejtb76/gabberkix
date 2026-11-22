# GABBERKIX

**A hardcore kick designer for the M-VAVE FM-1.** A kick in three layers: a midbody for character, a punch for the
attack and a sub for the low end, pressed into one kick by groups driven hard into clippers.

> **Status: phase 6 of 6.** The three layers are in: the midbody (a synthesized 909 through two rounds of EQ and
> distortion), the punch (the hard tick at the start of a kick), pressed together in group A's clipper, and the sub
> (group A's lows, split off), both into TOTAL's clipper; played on every beat or pitched from the keys and MIDI, with
> sixteen presets, a randomizer, NOISE, PERF, ENV and BOUNCE (`kick-firmware-instructions.md`; DOUBLE, a doubled
> punch, left out: the punch and the clippers make hard transients already).

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) and felucca-bsp, from Felucca by Leo Kuroshita
(Hügelton Instruments).

## The sound so far

```
midbody: 909 or a preset's kick (BOUNCE) -> EQ (4 peaks) -> DIST1 -> EQ2 (4 peaks) -> DIST2 -> POST (2 peaks) --+
punch:   a kick (this one, or a preset) -> GATE -> P.EQ (4 peaks) -> DRIVE -> LEVEL -+-> GROUP A: EQ -> CLIP -> OUT
GROUP A -+-> highpass --------------------------------+
         +-> lowpass -> SUB (envelope, drive, gain) --+-> PERF -> TOTAL: EQ -> CLIP -> OUT -> ENV -> limiter
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
- **SUB**: the kick's own lows, split from group A at X.FREQ (40–250 Hz, or OFF) by a Linkwitz-Riley crossover, so
  the sub falls exactly into the gap the highpass leaves and the two halves sum flat. The sub does not go through
  group A's clipper again. S.GAIN its level, S.HOLD and S.DECAY an envelope of its own over the kick's (OPEN: none),
  S.DRV a soft saturation.
- **TOTAL**: group A's highs and the sub together: an EQ of four peaks, then a clipper (T.PUSH, T.CLIP HARD, SOFT or
  OFF, T.OUT), where pushing the sub over full scale glues it to the rest.
- **SRC**, BOUNCE: the midbody's source, the 909 or a saved preset's whole kick (P1–P16: the keys that hold one;
  with none saved yet it stays on the 909). The preset is rendered once into a buffer (up to a second; again when it
  is saved anew, and on loading a preset that uses one) and played as a sample, so the chain goes over a finished kick
  again: bounce it, add more effects, repeat. Choosing a preset blanks the chain (no distortion, EQs flat, no punch,
  the groups and the sub wires, the 909's envelope wide open), so you hear the preset as it was and build from there;
  SEL + ARP takes that back. PITCH plays the sample up or down (as a sampler: up is shorter), START leaves its first
  milliseconds out, S.LEVEL; the 909 page still shapes it (HOLD and DECAY its level, SWEEP a pitch dive, CLICK and
  NOISE on top), the keys and MIDI play it pitched. One level: a preset that itself bounces another plays with the 909
  as its source. Presets stay parameters alone: the sample is rendered again from them.
- **NOISE** (on the 909 page): noise in the 909, at its level, so it lives in the body and is silent between kicks:
  grit for the distortions, shaped by the EQs before them. 0 %: none (a new kick's).
- **PERF**, a page to play on while the kick runs, as on a console: three peaks just before TOTAL (as if they were
  TOTAL's own bands; TOTAL keeps its four), HEIGHT for all three (0: none) and PEAK1–PEAK3 each its frequency, to
  sweep through the kick's screaming middle; on the second row DRIVE moves both distortions' drive by ±12 dB, LENGTH
  the 909's DECAY by ±50 %. They are saved with the preset; ARP sets them back to neutral.
- **ENV**: an envelope over the whole kick: a line through four points, ATTACK at the start, BODY, MIDDLE and TAIL
  where they are set (>BODY, >MID, >TAIL, the second row), each -24 to +12 dB; it glides from one to the next, then
  stays at the tail's. All at 0 dB: none.
- **The master**: a peak limiter at -0.3 dBFS, then MASTER's volume.

**The dice** (ARP) draw an extreme curve on both EQs, as a kick's EQ is drawn: cuts deep (15–30 dB) and narrow, boosts
wide (9–24 dB); EQ's bands between 30 Hz and 1 kHz, where they steer the first distortion, but no cut at the root
(35–90 Hz: the kick's body stays), EQ2's between 100 Hz and 8 kHz, with no boost above 5 kHz and none over 12 dB above
2 kHz (not shrill). Each distortion gets a type, a drive (DIST1 6–30 dB, DIST2 milder, 0–15 dB) and now and then a
bias. The 909 gets a SWEEP of 1–5 octaves, a CLICK of 0–80 % and a DECAY of 200–900 ms, every length as likely (its
TUNE, SW.T, HOLD and LEVEL stay). The punch is off one time in two (towards the oldschool gabber kick); else it gets a
P.GAIN of -12 to +6 dB, a P.HOLD of 0–10 ms, a P.DECAY of 1–200 ms (every length as likely), a P.TUNE of ±12 and its
own curve on P.EQ (its SRC and P.DRV stay, and so does group A). The sub gets an X.FREQ of 60–200 Hz, an S.GAIN of -3
to +6 dB, an S.HOLD of 0–100 ms and, two times in three, an S.DECAY of 150–1500 ms (else OPEN); POST, TOTAL and PERF
go back to a new kick's (a colour that suits one throw may not suit the next). NOISE is off one time in two, else 1–20
%; ENV stays. Then the kick is rendered once, measured after the limiter, and DIST2's OUT set so it is as loud as a
new kick: never soft, never silent. The screen says RANDOM, mid-screen, while it throws. SEL + ARP takes the last
throw back.

A new kick is a classic gabber kick at once: DIST1 a soft TANH at 12 dB, DIST2 a GABBER at 6 dB with its OUT at -9 dB,
its own tick on top (SELF, 8 ms, +6 dB), group A pushed 6 dB into a hard clip, its OUT at -6 dB, split at 120 Hz with
the sub at 0 dB, TOTAL pushed 3 dB into a hard clip, its OUT at -3 dB: about as loud as the clean 909 (a distorted
kick is a near-square wave, far denser than a clean one). Turn an OUT up to push the next stage or the limiter, down
when its drive goes up.

Everything is computed in single-precision float, which the FM-1's FPU runs; inside the engine the signal may go far
above full scale, and only the master stops it. Every filter is one state-variable filter, and the curves on the
screen are computed from the same coefficients the sound uses (`tools/chain_test.c` checks they agree).

## Controls

| Control | What it does |
| --- | --- |
| **SELECT** | The page: SRC, 909, EQ, D1, EQ2, D2, POST, PNCH, P.EQ, GRP.A, SUB, TOTAL, ENV, PERF, and round again; in the order of the sound, so walking through them builds the kick |
| **HOME** | The 909 page; again: SRC, and back |
| **EDIT** | The EQ page; again: the next band |
| **FX** | D1; again: D2, and back |
| **ENV** | PNCH; again: P.EQ, and back |
| **SEQ** | GRP.A; again: its next band |
| **OCT−** | SUB |
| **OCT+** | TOTAL; again: its next band |
| **REC** | PERF; again: ENV, and back |
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
| **SRC** | SOURCE, PITCH, START, S.LEVEL | |
| **909** | SWEEP (how far above the root the pitch starts, in octaves), SW.T (how fast it falls), HOLD, DECAY (to -60 dB) | TUNE (the root, ±12 semitones from G1), CLICK, LEVEL, NOISE |
| **EQ**, **EQ2**, **POST**, **P.EQ** | BAND (1–4, 1–4, A–B, 1–4), and its GAIN, Q, FREQ | |
| **D1**, **D2** | TYPE, DRIVE, BIAS, OUT | MIX |
| **PNCH** | SRC, P.GAIN, P.HOLD, P.DECAY | P.DRV, P.TUNE |
| **GRP.A** | BAND (1–4), and its GAIN, Q, FREQ | PUSH, CLIP, A.OUT |
| **SUB** | X.FREQ, S.GAIN, S.HOLD, S.DECAY | S.DRV |
| **TOTAL** | BAND (1–4), and its GAIN, Q, FREQ | T.PUSH, T.CLIP, T.OUT |
| **ENV** | ATTACK, BODY, MIDDLE, TAIL (dB) | >BODY, >MID, >TAIL (ms) |
| **PERF** | HEIGHT, PEAK1, PEAK2, PEAK3 | DRIVE, LENGTH |

Along the top the tabs: the pages of the layer shown (the midbody's six, the punch's two), the one shown lit, and the
other layers by name (MID, PN, A, SUB, TOT, PERF; by their letter when the line is full). Each page has its own colour
(SRC grey, 909 red, EQ cyan, D1 magenta, EQ2 blue, D2 amber, POST green, PNCH yellow, P.EQ teal, GRP.A pink, SUB
violet, TOTAL white, ENV lime, PERF orange), so a glance tells where you are. Under the knobs the screen shows the
kick's first second (909), zoomed so its body fills the height (the zoom at the right, X2, X3: the tick at the start
may run into the edge); on an EQ page its bands as an EQ plugin draws them (each a numbered dot with its own faint
curve, the band the knobs turn lit, their sum the bright line) over the spectrum after the next distortion, where its
sweet spots show (on P.EQ the punch's, on GRP.A the spectrum after the clipper); on a distortion page the spectrum
after it; on PNCH the first 50 ms, the whole kick faint and the punch bright over it; on SUB the crossover's two
halves (the sub's with its gain) over group A's spectrum; on ENV the kick with ENV's level over it; on PERF the three
peaks' curve over the whole kick's spectrum.

## Presets

Sixteen kicks, one on each white key. Hold **SAVE** and press a white key to save the kick there; hold **SEL** and
press one to load it. While SEL or SAVE is held, the white keys that hold a preset light dim, and the one loaded or
saved last lights bright. The header shows it (P3), with a star once the kick has changed since. The presets are kept
in the FM-1's flash, outside the firmware: they survive a restart and an update. A preset is the parameters alone (no
audio); parameters are only ever added at the end of the list, so a preset saved now still loads in a later version. A
preset saved before the punch loads with no punch and group A a straight wire, one saved before the sub with no split
and TOTAL a straight wire, one saved before NOISE, PERF and ENV with all three neutral: each sounds as it did.

## The splash

At power-on the FM-1 shows the logo (`assets/splash.png`, 240 × 240) and the version. To change it: a new 240 × 240
PNG there, then `python -I tools/make_splash.py assets/splash.png app/splash.inc` (16 colours, two pixels to a
byte: 28.8 KB of flash).

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
