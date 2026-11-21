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
 *   909 -> PRE (filter with drive) -> EQ (8 peaks) -> FILTER (with drive, envelope, key tracking) -> DIST -> POST (2)
 * The 909 is a sine whose pitch falls from a sweep to the root, a click on top, a hold-and-decay envelope. Its two
 * voices (the kick that sounds and the one it cut off, fading) are summed, then the chain runs once on the sum. The
 * filters before the distortion choose what it distorts: that is where the kick's character is made. Every filter is
 * one state-variable filter (the trapezoidal SVF), whose coefficients also give the curve the screen draws.
 *
 * Use:
 *   kick_init()                          once, before anything else
 *   kick_params_default(p)               a blank 909 (every filter and the distortion neutral)
 *   kick_cook(p, c)                      parameters -> coefficients (main loop; a few exp2s and tans)
 *   kick_trigger(e, c, semis)            a kick at the root + semis (audio side); the old one fades out
 *   kick_render(e, c, out, n)            n mono samples, written (audio side); the chain follows c at once
 *   kick_render_one(c, semis, out, max)  one whole kick from silence, offline; returns its length in samples
 *   kick_curve(c, what, hz)              the gain of the PRE..FILTER or the POST filters at hz, dB (the screen)
 *   kick_band_db(c, what, b, hz)         one EQ or POST band's gain at hz, dB (the screen) */
#ifndef KICK_H
#define KICK_H
#include <stdint.h>

#define KICK_RATE 44100
#define KICK_ROOT_HZ 48.999429f          /* G1: the default root (TUNE 0) */
#define KICK_EQ_BANDS 8
#define KICK_POST_BANDS 2

/* an EQ band (a peak, as in a parametric EQ: at 0 dB it does nothing): its three parameters, from its first */
enum { KB_FREQ, KB_GAIN, KB_Q, KB_N };
/* the PRE's and the FILTER's types */
enum { KFT_OFF, KFT_LP, KFT_HP, KFT_BP, KFT_NOTCH, KFT_N };
/* the distortion's types */
enum { KDT_OFF, KDT_TANH, KDT_HARD, KDT_FOLD, KDT_GABBER, KDT_N };

/* the parameters, as the player sets them (whole numbers, in the units shown); a preset is this array */
enum {
    KP_TUNE,                             /* root, semitones from G1 */
    KP_SWEEP,                            /* the pitch sweep's start above the root, 1/100 octave */
    KP_SWEEP_T,                          /* the sweep's time constant, ms */
    KP_HOLD,                             /* amplitude: held at full, ms */
    KP_DECAY,                            /* amplitude: then down to -60 dB in, ms */
    KP_CLICK,                            /* the click on the attack, % */
    KP_LEVEL,                            /* the layer's level, dB */
    KP_PRE_TYPE,                         /* PRE: KFT_* */
    KP_PRE_FREQ,                         /* Hz */
    KP_PRE_RESO,                         /* % (0: Q 0.71, 100: Q 45) */
    KP_PRE_DRIVE,                        /* dB into a tanh before the filter; 0: none */
    KP_EQ,                               /* EQ band b's parameter j: KP_EQ + KB_N * b + j (FREQ Hz, GAIN dB, */
    KP_EQ_END = KP_EQ + KB_N * KICK_EQ_BANDS,   /* Q 1/100) */
    KP_FLT_TYPE = KP_EQ_END,             /* FILTER: KFT_* */
    KP_FLT_FREQ,                         /* the cutoff, Hz */
    KP_FLT_RESO,                         /* % */
    KP_FLT_DRIVE,                        /* dB, as PRE's */
    KP_FLT_ENV,                          /* the cutoff starts this far above, 1/100 octave, and falls back */
    KP_FLT_ENV_T,                        /* in this time constant, ms */
    KP_FLT_KEY,                          /* the cutoff follows the kick's pitch, % */
    KP_DIST_TYPE,                        /* DIST: KDT_* */
    KP_DIST_DRIVE,                       /* dB in */
    KP_DIST_BIAS,                        /* an offset before the shaper, %: asymmetry, even harmonics */
    KP_DIST_OUT,                         /* dB out */
    KP_DIST_MIX,                         /* % wet */
    KP_POST,                             /* POST band b: KP_POST + KB_N * b + j, as the EQ's */
    KP_POST_END = KP_POST + KB_N * KICK_POST_BANDS,
    KP_COUNT = KP_POST_END
};

typedef struct { int16_t v[KP_COUNT]; } kick_params_t;

typedef struct {
    const char *name;                    /* on the screen, up to 6 characters; unique (kick_render takes it) */
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

/* coefficients: what the render needs, cooked from the parameters */
typedef struct {                         /* one state-variable filter: out = m0 * in + m1 * band + m2 * low */
    float g, k;                          /* tan(pi f / rate), 1 / Q */
    float a1, a2, a3;                    /* the update's, from g and k */
    float m0, m1, m2;
    uint8_t on;                          /* 0: it passes the signal unchanged, and is skipped */
} kick_svf_t;

typedef struct {
    float inc0;                          /* the root's phase step (turns a sample) */
    float sweep_ratio;                   /* start frequency / root - 1 */
    float sweep_k;                       /* the sweep's decay per sample */
    uint32_t hold;                       /* samples */
    float decay_k;                       /* the amplitude's decay per sample after the hold */
    float click;                         /* the click's level */
    float level;                         /* linear */
    int tune;                            /* semitones (the FILTER's key tracking counts from G1) */
    kick_svf_t pre;
    float pre_drive;                     /* linear; 0: no drive */
    kick_svf_t eq[KICK_EQ_BANDS];
    kick_svf_t flt;                      /* g at the cutoff, with no envelope and no key tracking */
    float flt_f;                         /* the cutoff / rate */
    float flt_drive;
    float flt_env;                       /* octaves */
    float flt_env_k;                     /* the envelope's decay per FLT_STEP samples */
    float flt_key;                       /* 0..1 */
    uint8_t dist;                        /* KDT_* */
    float dist_in, dist_bias, dist_zero, dist_out, dist_mix;   /* dist_zero: the shaper at the bias (taken off) */
    uint8_t dist_dc;                     /* the DC blocker after it (a bias, or GABBER's even harmonics) */
    kick_svf_t post[KICK_POST_BANDS];
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
    float pre[2], eq[KICK_EQ_BANDS][2], flt[2], post[KICK_POST_BANDS][2];
    float env;                           /* the FILTER's envelope, 1 at a kick, falling */
    int semis;                           /* the last kick's, for key tracking */
    float dc_x, dc_y;                    /* the DC blocker after the distortion */
    uint32_t quiet;                      /* samples the chain's output has been silent */
} kick_chain_t;

typedef struct {
    kick_voice_t v[2];                   /* the kick that sounds, and the one before it while it fades */
    uint8_t cur;
    kick_chain_t ch;
} kick_engine_t;

void kick_engine_reset(kick_engine_t *e);
void kick_trigger(kick_engine_t *e, const kick_coefs_t *c, int semis);
void kick_render(kick_engine_t *e, const kick_coefs_t *c, float *out, uint32_t n);
uint32_t kick_render_one(const kick_coefs_t *c, int semis, float *out, uint32_t max);
int kick_sounding(const kick_engine_t *e);
void kick_chain(kick_engine_t *e, const kick_coefs_t *c, float *buf, uint32_t n);   /* the chain alone, in place
                                                                                       (the tests' sines) */
enum { KC_PRE, KC_POST };                /* kick_curve: PRE, EQ and FILTER (what the distortion gets), or POST */
float kick_curve(const kick_coefs_t *c, int what, float hz);
float kick_band_db(const kick_coefs_t *c, int what, int band, float hz);   /* KC_PRE: an EQ band; KC_POST: POST's */

#endif
