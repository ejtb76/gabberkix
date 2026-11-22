/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The kick engine: everything that makes the sound, with no platform in it. The same file runs in the FM-1's audio
 * interrupt, in fm1-base's host simulator, in the browser (the edit site renders a preset's WAV with it) and in
 * tools/kick_render (the offline render and its checks).
 *
 * Float throughout (the FM-1's FPU is single precision; every build uses -ffp-contract=off, so all of them compute
 * the same samples). 1.0 is full scale; inside the engine the signal may go far above it, only the master stops it.
 *
 * The midbody so far:
 *   909 -> EQ (8 peaks) -> DIST 1 -> EQ 2 (4 peaks) -> DIST 2 -> POST (2 peaks)        then the master: a limiter
 * Extreme boosts and cuts before a distortion choose what it does; it makes overtones; the next EQ finds the sweet
 * spots among them, and the next distortion presses them together. An EQ alone changes little: EQs in a row are one
 * EQ, so the distortions between them are what make the variety. The 909 is a sine whose pitch falls from a sweep to
 * the root, a click on top, a hold-and-decay envelope. Its two voices (the kick that sounds and the one it cut off,
 * fading) are summed, then the chain runs once on the sum. Every filter is one state-variable filter (the
 * trapezoidal SVF), whose coefficients also give the curves the screen draws.
 *
 * Use:
 *   kick_init()                          once, before anything else
 *   kick_params_default(p)               a new kick: the EQs flat, a classic gabber kick from the distortions
 *   kick_cook(p, c)                      parameters -> coefficients (main loop; a few exp2s and tans)
 *   kick_trigger(e, c, semis)            a kick at the root + semis (audio side); the old one fades out
 *   kick_render(e, c, out, n)            n mono samples, written (audio side); the chain follows c at once
 *   kick_render_one(c, semis, out, max)  one whole kick from silence, offline; returns its length in samples
 *   kick_master(env, buf, n)             the master: a peak limiter at -0.3 dBFS (env: its state, 0 at the start)
 *   kick_curve(c, eq, hz)                an EQ's gain at hz, dB (KE_*: the screen)
 *   kick_band_db(c, eq, b, hz)           one of its bands' */
#ifndef KICK_H
#define KICK_H
#include <stdint.h>

#define KICK_RATE 44100
#define KICK_ROOT_HZ 48.999429f          /* G1: the default root (TUNE 0) */

/* the EQs: each a parametric EQ of peaks (at 0 dB a band does nothing) */
enum { KE_EQ1, KE_EQ2, KE_POST, KE_N };
#define KICK_EQ1_BANDS 8
#define KICK_EQ2_BANDS 4
#define KICK_POST_BANDS 2
/* a band's three parameters, from its first */
enum { KB_FREQ, KB_GAIN, KB_Q, KB_N };
/* a distortion's five, from its first */
enum { KD_TYPE, KD_DRIVE, KD_BIAS, KD_OUT, KD_MIX, KD_N };
/* its types */
enum { KDT_OFF, KDT_TANH, KDT_HARD, KDT_GABBER, KDT_N };

/* the parameters, as the player sets them (whole numbers, in the units shown); a preset is this array */
enum {
    KP_TUNE,                             /* root, semitones from G1 */
    KP_SWEEP,                            /* the pitch sweep's start above the root, 1/100 octave */
    KP_SWEEP_T,                          /* the sweep's time constant, ms */
    KP_HOLD,                             /* amplitude: held at full, ms */
    KP_DECAY,                            /* amplitude: then down to -60 dB in, ms */
    KP_CLICK,                            /* the click on the attack, % */
    KP_LEVEL,                            /* the layer's level, dB */
    KP_EQ1,                              /* EQ 1, band b's parameter j: KP_EQ1 + KB_N * b + j (FREQ Hz, GAIN dB, */
    KP_D1 = KP_EQ1 + KB_N * KICK_EQ1_BANDS,   /* Q 1/100); DIST 1's parameter j: KP_D1 + j (TYPE KDT_*, DRIVE dB */
    KP_EQ2 = KP_D1 + KD_N,               /* in, BIAS % (an offset before the shaper: asymmetry), OUT dB, MIX % wet) */
    KP_D2 = KP_EQ2 + KB_N * KICK_EQ2_BANDS,
    KP_POST = KP_D2 + KD_N,
    KP_COUNT = KP_POST + KB_N * KICK_POST_BANDS
};

typedef struct { int16_t v[KP_COUNT]; } kick_params_t;

typedef struct {
    const char *name;                    /* on the screen; unique (kick_render takes it) */
    int16_t min, max, def;
    uint8_t unit;                        /* KU_*: how kick_param_text writes it */
    uint8_t step;                        /* a detent: 0 = about 3 % of the value (times, Hz, Q), else this many units */
    const char *const *list;             /* KU_LIST: a name for each value from min */
} kick_param_info_t;
enum { KU_ST, KU_OCT, KU_MS, KU_PCT, KU_DB, KU_HZ, KU_Q, KU_LIST };

const kick_param_info_t *kick_param_info(int i);
void kick_param_text(int i, int v, char *buf);   /* the value with its unit, up to 8 characters */
int kick_param_step(int i, int v, int detents);  /* v moved by detents, clamped */
void kick_params_default(kick_params_t *p);
int kick_eq_bands(int eq);               /* KE_*: how many bands */
int kick_eq_param(int eq);               /* its first band's first parameter */

/* coefficients: what the render needs, cooked from the parameters */
typedef struct {                         /* one state-variable filter: out = m0 * in + m1 * band + m2 * low */
    float g, k;                          /* tan(pi f / rate), 1 / Q */
    float a1, a2, a3;                    /* the update's, from g and k */
    float m0, m1, m2;
    uint8_t on;                          /* 0: it passes the signal unchanged, and is skipped */
} kick_svf_t;

typedef struct {
    uint8_t type;                        /* KDT_* */
    uint8_t dc;                          /* the DC blocker after it (a bias, or GABBER's even harmonics) */
    float in, bias, zero, out, mix;      /* zero: the shaper at the bias (taken off) */
} kick_dist_t;

typedef struct {
    float inc0;                          /* the root's phase step (turns a sample) */
    float sweep_ratio;                   /* start frequency / root - 1 */
    float sweep_k;                       /* the sweep's decay per sample */
    uint32_t hold;                       /* samples */
    float decay_k;                       /* the amplitude's decay per sample after the hold */
    float click;                         /* the click's level */
    float level;                         /* linear */
    kick_svf_t eq1[KICK_EQ1_BANDS], eq2[KICK_EQ2_BANDS], post[KICK_POST_BANDS];
    kick_dist_t d1, d2;
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

typedef struct {                         /* the chain's state: it rings on after the voices stop */
    float eq1[KICK_EQ1_BANDS][2], eq2[KICK_EQ2_BANDS][2], post[KICK_POST_BANDS][2];
    float dc[2][2];                      /* the DC blockers after the distortions: x1, y1 */
    uint32_t quiet;                      /* samples the chain's output has been silent */
} kick_chain_t;

/* where the chain's output is taken: all of it, or just after a distortion (the screen's spectra) */
enum { KT_ALL, KT_D1, KT_D2 };

typedef struct {
    kick_voice_t v[2];                   /* the kick that sounds, and the one before it while it fades */
    uint8_t cur;
    uint8_t tap;                         /* KT_*: KT_ALL after kick_engine_reset */
    kick_chain_t ch;
} kick_engine_t;

void kick_engine_reset(kick_engine_t *e);
void kick_trigger(kick_engine_t *e, const kick_coefs_t *c, int semis);
void kick_render(kick_engine_t *e, const kick_coefs_t *c, float *out, uint32_t n);
uint32_t kick_render_one(const kick_coefs_t *c, int semis, float *out, uint32_t max);
int kick_sounding(const kick_engine_t *e);
void kick_master(float *env, float *buf, uint32_t n);
void kick_chain(kick_engine_t *e, const kick_coefs_t *c, float *buf, uint32_t n);   /* the chain alone, in place
                                                                                       (the tests' sines) */
float kick_curve(const kick_coefs_t *c, int eq, float hz);
float kick_band_db(const kick_coefs_t *c, int eq, int band, float hz);

#endif
