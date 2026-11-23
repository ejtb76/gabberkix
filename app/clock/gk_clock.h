/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead (FMLarynx's larynx_clock, Amenity's jungle_clock: the same loop) */
/* Following MIDI clock, as Amenity does (its jungle_clock.c, the same delay-locked loop): gk_clock.c takes the
 * pulses with their time stamps in the 1 kHz tick and filters them into a smooth time line; the audio interrupt
 * (app.c) places the beat's pulses on that line, to the sample. */
#ifndef GK_CLOCK_H
#define GK_CLOCK_H
#include <stdint.h>

#define GK_TICK_HZ 24000000U   /* the time stamps' clock: the platform's 24 MHz timer */

enum { CLOCK_OFF, CLOCK_ARMED, CLOCK_RUNNING, CLOCK_STOPPED };

/* What the loop has found, published with a sequence count (odd while it is
 * written): the time of pulse `pulse` (0: the first after Start, step 1) is
 * t0 + t0_frac ticks, pulses come `per` ticks apart (inv_per = 1 / per).
 * `start` counts Starts and Continues; `resume` is 1 after a Continue. */
struct gk_follow {
    uint32_t seq, t0, pulse;
    float t0_frac, per, inv_per;
    uint8_t state, start, resume;
};
/* The audio interrupt's copy: the latest consistent one. */
void gk_follow_read(struct gk_follow *f);

/* The tick passes every realtime status here (F8 clock, FA start, FB continue, FC stop) with the input it came from
 * (plat_midi_in_from) and the time it came in (GK_TICK_HZ). Then gk_clock, with the time now: 1 while it
 * follows (half a second without pulses: 0, the internal clock again), and the tempo found (BPM x 10). */
void gk_midi_realtime(unsigned status, unsigned from, uint32_t t);
int gk_clock(unsigned *bpm_x10, uint32_t now);
/* the loop's period now (ticks of GK_TICK_HZ a pulse; 0 when not following) */
float gk_clock_period(void);
/* the app's tempo (BPM x 10): the starting guess when nothing has been timed before a Start */
unsigned gk_tempo_x10(void);

/* the readout (the app's app_log, tools fm1_remote.py log): a clock followed and lost (half a second without pulses)
 * and how long it had been, the loop started again far out (over six pulses), since the last read */
struct gk_clock_diag { uint32_t lost, lost_ms, far; };
extern volatile struct gk_clock_diag gk_clock_diag;

#endif
