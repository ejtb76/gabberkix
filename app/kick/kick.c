/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The kick engine (kick.h). Phase 1: the synthesized 909 that is the midbody's source. */
#include "kick.h"
#include "fastmath.h"

#define MS (KICK_RATE / 1000.0f)         /* samples a millisecond */
#define FADE_SAMPLES 64.0f               /* a kick cut off by the next one fades out in 1.5 ms: no click */
#define CLICK_TAU_MS 1.2f                /* the click's noise burst */
#define SILENT 1e-5f                     /* -100 dB: a voice below it is done */

static const kick_param_info_t INFO[KP_COUNT] = {
    [KP_TUNE] = {"TUNE", -12, 12, 0, KU_ST, 1},
    [KP_SWEEP] = {"SWEEP", 0, 600, 200, KU_OCT, 5},
    [KP_SWEEP_T] = {"SW.T", 1, 500, 30, KU_MS, 0},
    [KP_HOLD] = {"HOLD", 0, 1000, 20, KU_MS, 0},
    [KP_DECAY] = {"DECAY", 10, 4000, 450, KU_MS, 0},
    [KP_CLICK] = {"CLICK", 0, 100, 25, KU_PCT, 2},
    [KP_LEVEL] = {"LEVEL", -36, 12, 0, KU_DB, 1},
};

const kick_param_info_t *kick_param_info(int i) { return i >= 0 && i < KP_COUNT ? &INFO[i] : 0; }

/* ------------------------------------------------------------------ text --- */
static char *put_uint(char *b, unsigned u)
{
    char t[10];
    int n = 0;
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 10);
    while (n)
        *b++ = t[--n];
    return b;
}
static char *put_str(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    return b;
}

void kick_param_text(int i, int v, char *b)
{
    const kick_param_info_t *p = kick_param_info(i);
    unsigned u = (unsigned)(v < 0 ? -v : v);
    if (!p) {
        *b = 0;
        return;
    }
    if (v < 0)
        *b++ = '-';
    else if (p->unit == KU_ST || p->unit == KU_DB)
        *b++ = v ? '+' : ' ';
    switch (p->unit) {
    case KU_OCT:                         /* 1/100 octave: "2.00" */
        b = put_uint(b, u / 100u);
        *b++ = '.';
        *b++ = (char)('0' + u / 10u % 10u);
        *b++ = (char)('0' + u % 10u);
        b = put_str(b, "oc");
        break;
    case KU_MS:
        if (u >= 1000u) {                /* "1.25s" */
            b = put_uint(b, u / 1000u);
            *b++ = '.';
            *b++ = (char)('0' + u / 100u % 10u);
            *b++ = (char)('0' + u / 10u % 10u);
            *b++ = 's';
        } else {
            b = put_uint(b, u);
            b = put_str(b, "ms");
        }
        break;
    case KU_PCT:
        b = put_uint(b, u);
        *b++ = '%';
        break;
    case KU_DB:
        b = put_uint(b, u);
        b = put_str(b, "dB");
        break;
    default:                             /* KU_ST */
        b = put_uint(b, u);
        b = put_str(b, "st");
        break;
    }
    *b = 0;
}

int kick_param_step(int i, int v, int detents)
{
    const kick_param_info_t *p = kick_param_info(i);
    int k;
    if (!p)
        return v;
    for (k = 0; k < (detents < 0 ? -detents : detents); k++) {
        int d = p->step ? p->step : (v * 3 / 100 > 1 ? v * 3 / 100 : 1);   /* times: about 3 % a detent */
        v += detents < 0 ? -d : d;
        if (v <= p->min || v >= p->max)
            break;
    }
    return v < p->min ? p->min : v > p->max ? p->max : v;
}

void kick_params_default(kick_params_t *p)
{
    int i;
    for (i = 0; i < KP_COUNT; i++)
        p->v[i] = INFO[i].def;
}

/* ------------------------------------------------------------- the cook --- */
static float click_k;                   /* the click's decay per sample (kick_init) */

void kick_init(void) { click_k = fm_expf(-1.0f / (CLICK_TAU_MS * MS)); }

static float decay_per_sample(float ms, float db)   /* down by db over ms */
{
    return fm_expf(-db * 0.115129255f / (ms * MS));  /* ln(10) / 20 */
}

void kick_cook(const kick_params_t *p, kick_coefs_t *c)
{
    float f = KICK_ROOT_HZ * fm_exp2f((float)p->v[KP_TUNE] / 12.0f);
    c->inc0 = f / (float)KICK_RATE;
    c->sweep_ratio = fm_exp2f((float)p->v[KP_SWEEP] / 100.0f) - 1.0f;
    c->sweep_k = fm_expf(-1.0f / ((float)p->v[KP_SWEEP_T] * MS));
    c->hold = (uint32_t)((float)p->v[KP_HOLD] * MS);
    c->decay_k = decay_per_sample((float)p->v[KP_DECAY], 60.0f);
    c->click = (float)p->v[KP_CLICK] * 0.006f;   /* 100 % is a burst at 0.6 */
    c->level = fm_db2lin((float)p->v[KP_LEVEL]);
}

/* ----------------------------------------------------------- the voices --- */
void kick_engine_reset(kick_engine_t *e)
{
    e->v[0].on = e->v[1].on = 0;
    e->cur = 0;
}

static void voice_start(kick_voice_t *v, const kick_coefs_t *c, int semis)
{
    float r, top;
    if (semis < -24)
        semis = -24;
    else if (semis > 24)
        semis = 24;
    r = fm_exp2f((float)semis / 12.0f);
    v->inc0 = c->inc0 * r;
    v->inc_sweep = v->inc0 * c->sweep_ratio;
    top = 0.2f - v->inc0;                /* the sweep starts at 8.8 kHz at most */
    if (v->inc_sweep > top)
        v->inc_sweep = top;
    v->phase = 0.0f;                     /* from a zero crossing, rising: no click at the start */
    v->sweep = 1.0f;
    v->amp = 1.0f;
    v->hold = c->hold;
    v->decay_k = c->decay_k;
    v->sweep_k = c->sweep_k;
    v->click_env = 1.0f;
    v->click = c->click;
    v->level = c->level;
    v->noise = 0x12345678u;              /* the same click every time: a kick is a kick */
    v->fade = 1.0f;
    v->on = 1;
}

void kick_trigger(kick_engine_t *e, const kick_coefs_t *c, int semis)
{
    kick_voice_t *old = &e->v[e->cur];
    if (old->on && old->fade >= 1.0f)    /* the sounding kick fades out under the new one */
        old->fade = 0.999f;
    e->cur ^= 1u;
    voice_start(&e->v[e->cur], c, semis);
}

static void voice_render(kick_voice_t *v, float *out, uint32_t n)
{
    uint32_t i;
    float fade_step = 1.0f / FADE_SAMPLES;
    for (i = 0; i < n; i++) {
        float s, nz;
        float inc = v->inc0 + v->inc_sweep * v->sweep;
        v->sweep *= v->sweep_k;
        v->phase += inc;
        if (v->phase >= 1.0f)
            v->phase -= 1.0f;
        s = fm_sin_turns(v->phase) * v->amp;
        if (v->hold)
            v->hold--;
        else
            v->amp *= v->decay_k;
        v->noise = v->noise * 1664525u + 1013904223u;
        nz = (float)(int32_t)v->noise * (1.0f / 2147483648.0f);
        s += nz * v->click_env * v->click;
        v->click_env *= click_k;
        if (v->fade < 1.0f) {
            v->fade -= fade_step;
            if (v->fade <= 0.0f) {
                v->on = 0;
                return;
            }
            s *= v->fade;
        }
        out[i] += s * v->level;
    }
    if (v->sweep < 1e-9f)
        v->sweep = 0.0f;
    if (v->click_env < 1e-9f)
        v->click_env = 0.0f;
    if (!v->hold && v->amp < SILENT && v->click_env < SILENT)
        v->on = 0;
}

void kick_render(kick_engine_t *e, float *out, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        out[i] = 0.0f;
    for (i = 0; i < 2u; i++)
        if (e->v[i].on)
            voice_render(&e->v[i], out, n);
}

int kick_sounding(const kick_engine_t *e) { return e->v[0].on || e->v[1].on; }

uint32_t kick_render_one(const kick_coefs_t *c, int semis, float *out, uint32_t max)
{
    kick_engine_t e;
    uint32_t done = 0;
    kick_engine_reset(&e);
    kick_trigger(&e, c, semis);
    while (done < max && kick_sounding(&e)) {
        uint32_t n = max - done < 256u ? max - done : 256u;
        kick_render(&e, out + done, n);
        done += n;
    }
    return done;
}
