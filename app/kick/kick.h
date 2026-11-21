/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The kick engine: everything that makes the sound, with no platform in it. The same file runs in the FM-1's audio
 * interrupt, in fm1-base's host simulator, in the browser (the edit site renders a preset's WAV with it) and in
 * tools/kick_render (the offline render and its checks).
 *
 * Float throughout (the FM-1's FPU is single precision; every build uses -ffp-contract=off, so all of them compute
 * the same samples). 1.0 is full scale; inside the engine the signal may go far above it, only the master stops it.
 *
 * Phase 1: the midbody's source, a synthesized 909 (a sine whose pitch falls from a sweep to the root, a click on
 * top, a hold-and-decay amplitude envelope). Later phases add the chain behind it.
 *
 * Use:
 *   kick_init()                          once, before anything else
 *   kick_params_default(p)               a blank 909
 *   kick_cook(p, c)                      parameters -> coefficients (main loop; cheap, a few exp2s)
 *   kick_trigger(e, c, semis)            a kick at the root + semis (audio side); the old one fades out
 *   kick_render(e, out, n)               n mono samples, written (audio side)
 *   kick_render_one(c, semis, out, max)  one whole kick from silence, offline; returns its length in samples */
#ifndef KICK_H
#define KICK_H
#include <stdint.h>

#define KICK_RATE 44100
#define KICK_ROOT_HZ 48.999429f          /* G1: the default root (TUNE 0) */

/* the parameters, as the player sets them (whole numbers, in the units shown); a preset is this array */
enum {
    KP_TUNE,                             /* root, semitones from G1 */
    KP_SWEEP,                            /* the pitch sweep's start above the root, 1/100 octave */
    KP_SWEEP_T,                          /* the sweep's time constant, ms */
    KP_HOLD,                             /* amplitude: held at full, ms */
    KP_DECAY,                            /* amplitude: then down to -60 dB in, ms */
    KP_CLICK,                            /* the click on the attack, % */
    KP_LEVEL,                            /* the layer's level, dB */
    KP_COUNT
};

typedef struct { int16_t v[KP_COUNT]; } kick_params_t;

typedef struct {
    const char *name;                    /* on the screen, up to 6 characters */
    int16_t min, max, def;
    uint8_t unit;                        /* KU_*: how kick_param_text writes it */
    uint8_t step;                        /* a detent: 0 = about 3 % of the value (times), else this many units */
} kick_param_info_t;
enum { KU_ST, KU_OCT, KU_MS, KU_PCT, KU_DB };

const kick_param_info_t *kick_param_info(int i);
void kick_param_text(int i, int v, char *buf);   /* the value with its unit, up to 8 characters */
int kick_param_step(int i, int v, int detents);  /* v moved by detents, clamped */
void kick_params_default(kick_params_t *p);

/* coefficients: what the render needs, cooked from the parameters */
typedef struct {
    float inc0;                          /* the root's phase step (turns a sample) */
    float sweep_ratio;                   /* start frequency / root - 1 */
    float sweep_k;                       /* the sweep's decay per sample */
    uint32_t hold;                       /* samples */
    float decay_k;                       /* the amplitude's decay per sample after the hold */
    float click;                         /* the click's level */
    float level;                         /* linear */
} kick_coefs_t;

void kick_init(void);
void kick_cook(const kick_params_t *p, kick_coefs_t *c);

typedef struct {
    uint8_t on;
    float inc0, inc_sweep;               /* phase step: inc0 + inc_sweep * sweep */
    float phase, sweep, amp, click_env;
    uint32_t hold;
    float decay_k, sweep_k, click, level;
    uint32_t noise;
    float fade;                          /* 1, or falling to 0 when a new kick took over */
} kick_voice_t;

typedef struct {
    kick_voice_t v[2];                   /* the kick that sounds, and the one before it while it fades */
    uint8_t cur;
} kick_engine_t;

void kick_engine_reset(kick_engine_t *e);
void kick_trigger(kick_engine_t *e, const kick_coefs_t *c, int semis);
void kick_render(kick_engine_t *e, float *out, uint32_t n);
uint32_t kick_render_one(const kick_coefs_t *c, int semis, float *out, uint32_t max);
int kick_sounding(const kick_engine_t *e);

#endif
