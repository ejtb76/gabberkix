/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The kick engine (kick.h): the synthesized 909 and the midbody's chain behind it. */
#include "kick.h"
#include "fastmath.h"

#define MS (KICK_RATE / 1000.0f)         /* samples a millisecond */
#define FADE_SAMPLES 64.0f               /* a kick cut off by the next one fades out in 1.5 ms: no click */
#define CLICK_TAU_MS 1.2f                /* the click's noise burst */
#define SILENT 1e-5f                     /* -100 dB: a voice below it is done */
#define QUIET_SAMPLES 2048u              /* the chain is done when its output was silent this long (46 ms) */
#define F_MIN 20.0f
#define F_MAX (0.45f * KICK_RATE)        /* 19.8 kHz: the SVF's tan stays far from its pole */
#define DC_R 0.99929f                    /* the DC blockers after the distortions: 5 Hz */
#define RND_LOUD -11.0f                  /* kick_randomize: a new kick's loudness, dBFS, */
#define RND_SPREAD 1.5f                  /* this close, */
#define RND_TRIES 8                      /* in at most so many throws */
#define D2_OUT -9                        /* DIST 2's OUT on a new kick: as loud as the clean 909 (chain_test) */

/* ------------------------------------------------------------ parameters --- */
static const char *const DIST_T[KDT_N] = {"OFF", "TANH", "HARD", "GABBER"};

/* an EQ band, at its first parameter: a gain-0 peak at f (so it does nothing until its GAIN moves), Q 1 */
#define BAND(at, n, f)                                                                                               \
    [(at) + KB_FREQ] = {"FREQ" n, 20, 19800, f, KU_HZ, 0, 0},                                                        \
    [(at) + KB_GAIN] = {"GAIN" n, -30, 30, 0, KU_DB, 1, 0},                                                          \
    [(at) + KB_Q] = {"Q" n, 10, 3000, 100, KU_Q, 0, 0}

/* a distortion, at its first parameter */
#define DIST(at, n, type, drive, out)                                                                                \
    [(at) + KD_TYPE] = {"TYPE" n, 0, KDT_N - 1, type, KU_LIST, 1, DIST_T},                                           \
    [(at) + KD_DRIVE] = {"DRIVE" n, 0, 60, drive, KU_DB, 1, 0},                                                      \
    [(at) + KD_BIAS] = {"BIAS" n, -100, 100, 0, KU_PCT, 5, 0},                                                       \
    [(at) + KD_OUT] = {"OUT" n, -36, 12, out, KU_DB, 1, 0},                                                          \
    [(at) + KD_MIX] = {"MIX" n, 0, 100, 100, KU_PCT, 5, 0}

static const kick_param_info_t INFO[KP_COUNT] = {
    [KP_TUNE] = {"TUNE", -12, 12, 0, KU_ST, 1, 0},
    [KP_SWEEP] = {"SWEEP", 0, 600, 200, KU_OCT, 5, 0},
    [KP_SWEEP_T] = {"SW.T", 1, 500, 30, KU_MS, 0, 0},
    [KP_HOLD] = {"HOLD", 0, 1000, 20, KU_MS, 0, 0},
    [KP_DECAY] = {"DECAY", 10, 4000, 450, KU_MS, 0, 0},
    [KP_CLICK] = {"CLICK", 0, 100, 25, KU_PCT, 2, 0},
    [KP_LEVEL] = {"LEVEL", -36, 12, 0, KU_DB, 1, 0},
    BAND(KP_EQ1 + 0 * KB_N, "1", 50),          /* low: before the first distortion the lows matter most */
    BAND(KP_EQ1 + 1 * KB_N, "2", 100),
    BAND(KP_EQ1 + 2 * KB_N, "3", 200),
    BAND(KP_EQ1 + 3 * KB_N, "4", 500),
    DIST(KP_D1, "1", KDT_TANH, 12, 0),       /* a new kick: a soft drive, then a gabber clip */
    BAND(KP_EQ2 + 0 * KB_N, "2.1", 100),
    BAND(KP_EQ2 + 1 * KB_N, "2.2", 300),
    BAND(KP_EQ2 + 2 * KB_N, "2.3", 1000),
    BAND(KP_EQ2 + 3 * KB_N, "2.4", 3000),
    DIST(KP_D2, "2", KDT_GABBER, 6, D2_OUT),
    BAND(KP_POST + 0 * KB_N, "A", 60),
    BAND(KP_POST + 1 * KB_N, "B", 3000),
};

int kick_eq_bands(int eq) { return eq == KE_EQ1 ? KICK_EQ1_BANDS : eq == KE_EQ2 ? KICK_EQ2_BANDS : KICK_POST_BANDS; }
int kick_eq_param(int eq) { return eq == KE_EQ1 ? KP_EQ1 : eq == KE_EQ2 ? KP_EQ2 : KP_POST; }


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
    if (p->unit == KU_LIST) {
        int k = v - p->min;
        put_str(b, k >= 0 && k <= p->max - p->min ? p->list[k] : "?")[0] = 0;
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
    case KU_HZ:                          /* "49Hz", "1.25k", "12.5k" */
        if (u >= 10000u) {
            b = put_uint(b, u / 1000u);
            *b++ = '.';
            *b++ = (char)('0' + u / 100u % 10u);
            *b++ = 'k';
        } else if (u >= 1000u) {
            b = put_uint(b, u / 1000u);
            *b++ = '.';
            *b++ = (char)('0' + u / 100u % 10u);
            *b++ = (char)('0' + u / 10u % 10u);
            *b++ = 'k';
        } else {
            b = put_uint(b, u);
            b = put_str(b, "Hz");
        }
        break;
    case KU_Q:                           /* 1/100: "0.71", "30.0" */
        b = put_uint(b, u / 100u);
        *b++ = '.';
        *b++ = (char)('0' + u / 10u % 10u);
        if (u < 1000u)
            *b++ = (char)('0' + u % 10u);
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

static float clamp_f(float hz) { return hz < F_MIN ? F_MIN : hz > F_MAX ? F_MAX : hz; }

static void svf_update(kick_svf_t *s)    /* a1..a3 from g and k */
{
    s->a1 = 1.0f / (1.0f + s->g * (s->g + s->k));
    s->a2 = s->g * s->a1;
    s->a3 = s->g * s->a2;
}

/* an EQ band: a peak, +-db at f, its width Q (Andrew Simper, "Linear trapezoidal integrated SVF", 2013). At 0 dB it
 * passes the signal unchanged and is skipped. */
static void cook_band(kick_svf_t *s, const int16_t *b)
{
    float q = (float)b[KB_Q] * 0.01f;
    float a = fm_db2lin((float)b[KB_GAIN] * 0.5f);   /* sqrt of the gain */
    s->on = b[KB_GAIN] != 0;
    s->g = fm_tanf(FM_PI * clamp_f((float)b[KB_FREQ]) / (float)KICK_RATE);
    s->k = 1.0f / (q * a);
    s->m0 = 1.0f;
    s->m1 = s->k * (a * a - 1.0f);
    s->m2 = 0.0f;
    svf_update(s);
}

static float shape(int type, float u)    /* the distortion's transfer: 1 is where it starts to bite */
{
    switch (type) {
    case KDT_TANH:
        return fm_tanhf(u);
    case KDT_HARD:
        return u > 1.0f ? 1.0f : u < -1.0f ? -1.0f : u;
    case KDT_GABBER: {                   /* a soft stage 12 dB hotter, a square on top (even harmonics, the buzz), */
        float s = fm_tanhf(4.0f * u);    /* then a hard clip */
        s = (s + 0.3f * s * s) * 1.4f;
        return s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
    }
    default:
        return u;
    }
}

static void cook_dist(kick_dist_t *d, const int16_t *v)
{
    d->type = (uint8_t)v[KD_TYPE];
    d->in = fm_db2lin((float)v[KD_DRIVE]);
    d->bias = (float)v[KD_BIAS] * 0.005f;   /* 100 %: half way to where the shaper bites */
    d->zero = shape(d->type, d->bias);
    d->dc = v[KD_BIAS] != 0 || d->type == KDT_GABBER;   /* the shapers that leave a DC offset */
    d->out = fm_db2lin((float)v[KD_OUT]);
    d->mix = (float)v[KD_MIX] * 0.01f;
}

void kick_cook(const kick_params_t *p, kick_coefs_t *c)
{
    const int16_t *v = p->v;
    int b;
    float f = KICK_ROOT_HZ * fm_exp2f((float)v[KP_TUNE] / 12.0f);
    c->inc0 = f / (float)KICK_RATE;
    c->sweep_ratio = fm_exp2f((float)v[KP_SWEEP] / 100.0f) - 1.0f;
    c->sweep_k = fm_expf(-1.0f / ((float)v[KP_SWEEP_T] * MS));
    c->hold = (uint32_t)((float)v[KP_HOLD] * MS);
    c->decay_k = decay_per_sample((float)v[KP_DECAY], 60.0f);
    c->click = (float)v[KP_CLICK] * 0.006f;   /* 100 % is a burst at 0.6 */
    c->level = fm_db2lin((float)v[KP_LEVEL]);
    for (b = 0; b < KICK_EQ1_BANDS; b++)
        cook_band(&c->eq1[b], &v[KP_EQ1 + KB_N * b]);
    cook_dist(&c->d1, &v[KP_D1]);
    for (b = 0; b < KICK_EQ2_BANDS; b++)
        cook_band(&c->eq2[b], &v[KP_EQ2 + KB_N * b]);
    cook_dist(&c->d2, &v[KP_D2]);
    for (b = 0; b < KICK_POST_BANDS; b++)
        cook_band(&c->post[b], &v[KP_POST + KB_N * b]);
}

/* the gain of one filter at hz: its digital response, exactly (the bilinear map: s = j tan(pi f / rate) / g) */
static float svf_db(const kick_svf_t *s, float t)   /* t = tan(pi hz / rate) */
{
    float x = t / s->g, x2 = x * x;
    float nr = s->m0 * (1.0f - x2) + s->m2, ni = x * (s->m0 * s->k + s->m1);
    float dr = 1.0f - x2, di = s->k * x;
    float num = nr * nr + ni * ni, den = dr * dr + di * di;
    if (num < 1e-12f * den)
        return -120.0f;
    return 3.01029996f * fm_log2f(num / den);      /* 10 log10 = 3.01 log2 */
}

static const kick_svf_t *eq_bands(const kick_coefs_t *c, int eq)
{
    return eq == KE_EQ1 ? c->eq1 : eq == KE_EQ2 ? c->eq2 : c->post;
}

float kick_band_db(const kick_coefs_t *c, int eq, int band, float hz)
{
    const kick_svf_t *s = &eq_bands(c, eq)[band];
    return s->on ? svf_db(s, fm_tanf(FM_PI * clamp_f(hz) / (float)KICK_RATE)) : 0.0f;
}

float kick_curve(const kick_coefs_t *c, int eq, float hz)
{
    const kick_svf_t *s = eq_bands(c, eq);
    float t = fm_tanf(FM_PI * clamp_f(hz) / (float)KICK_RATE), db = 0.0f;
    int b;
    for (b = 0; b < kick_eq_bands(eq); b++)
        if (s[b].on)
            db += svf_db(&s[b], t);
    return db;
}

/* ----------------------------------------------------------- the voices --- */
static void chain_reset(kick_chain_t *ch)
{
    float *z = &ch->eq1[0][0];
    uint32_t i;
    for (i = 0; i < (uint32_t)(sizeof ch->eq1 + sizeof ch->eq2 + sizeof ch->post + sizeof ch->dc) / sizeof(float); i++)
        z[i] = 0.0f;                     /* (eq1, eq2, post and dc: floats, one after the other) */
    ch->quiet = QUIET_SAMPLES;           /* nothing to ring */
}

void kick_engine_reset(kick_engine_t *e)
{
    e->v[0].on = e->v[1].on = 0;
    e->cur = 0;
    e->tap = KT_ALL;
    chain_reset(&e->ch);
}

static void voice_start(kick_voice_t *v, const kick_coefs_t *c, int semis)
{
    float r, top;
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
    if (semis < -24)
        semis = -24;
    else if (semis > 24)
        semis = 24;
    if (old->on && old->fade >= 1.0f)    /* the sounding kick fades out under the new one */
        old->fade = 0.999f;
    e->cur ^= 1u;
    voice_start(&e->v[e->cur], c, semis);
    e->ch.quiet = 0;                     /* (the filters keep ringing under the new kick) */
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

/* ------------------------------------------------------------ the chain --- */
static void svf_run(const kick_svf_t *s, float *z, float *buf, uint32_t n)
{
    uint32_t i;
    float z0 = z[0], z1 = z[1];
    for (i = 0; i < n; i++) {
        float v0 = buf[i], v3 = v0 - z1;
        float v1 = s->a1 * z0 + s->a2 * v3;
        float v2 = z1 + s->a2 * z0 + s->a3 * v3;
        z0 = 2.0f * v1 - z0;
        z1 = 2.0f * v2 - z1;
        buf[i] = s->m0 * v0 + s->m1 * v1 + s->m2 * v2;
    }
    z[0] = fm_flush(z0);
    z[1] = fm_flush(z1);
}

static void dist_run(const kick_dist_t *d, float *dc, float *buf, uint32_t n)
{
    uint32_t i;
    int type = d->type;
    float x1 = dc[0], y1 = dc[1];
    for (i = 0; i < n; i++) {
        float x = buf[i];
        float w = (shape(type, x * d->in + d->bias) - d->zero) * d->out;
        float y = x + d->mix * (w - x);
        if (d->dc) {                     /* the bias leaves a DC offset while the kick sounds: off it goes */
            float o = y - x1 + DC_R * y1;
            x1 = y;
            y1 = o;
            y = o;
        }
        buf[i] = y;
    }
    dc[0] = fm_flush(x1);
    dc[1] = fm_flush(y1);
}

static void eq_run(const kick_svf_t *s, float (*z)[2], int nb, float *buf, uint32_t n)
{
    int b;
    for (b = 0; b < nb; b++)
        if (s[b].on)
            svf_run(&s[b], z[b], buf, n);
}

void kick_chain(kick_engine_t *e, const kick_coefs_t *c, float *buf, uint32_t n)
{
    kick_chain_t *ch = &e->ch;
    eq_run(c->eq1, ch->eq1, KICK_EQ1_BANDS, buf, n);
    if (c->d1.type != KDT_OFF)
        dist_run(&c->d1, ch->dc[0], buf, n);
    if (e->tap == KT_D1)
        return;
    eq_run(c->eq2, ch->eq2, KICK_EQ2_BANDS, buf, n);
    if (c->d2.type != KDT_OFF)
        dist_run(&c->d2, ch->dc[1], buf, n);
    if (e->tap == KT_D2)
        return;
    eq_run(c->post, ch->post, KICK_POST_BANDS, buf, n);
}

void kick_render(kick_engine_t *e, const kick_coefs_t *c, float *out, uint32_t n)
{
    uint32_t i;
    float peak = 0.0f;
    int voices;
    for (i = 0; i < n; i++)
        out[i] = 0.0f;
    for (i = 0; i < 2u; i++)
        if (e->v[i].on)
            voice_render(&e->v[i], out, n);
    voices = e->v[0].on || e->v[1].on;
    if (!voices && e->ch.quiet >= QUIET_SAMPLES)
        return;                          /* silence in, nothing ringing: the chain sleeps */
    kick_chain(e, c, out, n);
    for (i = 0; i < n; i++) {
        float a = fm_fabsf(out[i]);
        if (a > peak)
            peak = a;
    }
    if (voices || peak >= SILENT)
        e->ch.quiet = 0;
    else if ((e->ch.quiet += n) >= QUIET_SAMPLES)
        chain_reset(&e->ch);             /* rung out: from zero next time (no denormals, no old state) */
}

/* the master: a peak limiter (instant attack, so nothing passes the ceiling; 80 ms release) */
#define LIM_CEIL 0.966f                  /* -0.3 dBFS */
void kick_master(float *env, float *buf, uint32_t n)
{
    static float rel;
    float e = *env;
    uint32_t i;
    if (rel == 0.0f)
        rel = fm_expf(-1.0f / (80.0f * MS));
    for (i = 0; i < n; i++) {
        float a = fm_fabsf(buf[i]);
        e *= rel;                        /* falls, but never under the signal */
        if (a > e)
            e = a;
        if (e > LIM_CEIL)
            buf[i] *= LIM_CEIL / e;
    }
    *env = fm_flush(e);
}

int kick_sounding(const kick_engine_t *e) { return e->v[0].on || e->v[1].on || e->ch.quiet < QUIET_SAMPLES; }

uint32_t kick_render_one(const kick_coefs_t *c, int semis, float *out, uint32_t max)
{
    kick_engine_t e;
    uint32_t done = 0;
    kick_engine_reset(&e);
    kick_trigger(&e, c, semis);
    while (done < max && kick_sounding(&e)) {
        uint32_t n = max - done < 256u ? max - done : 256u;
        kick_render(&e, c, out + done, n);
        done += n;
    }
    return done;
}

/* ------------------------------------------------------------- the dice --- */
/* how loud a kick is: one beat at 160 BPM through the master's limiter, its mean square, dBFS */
float kick_loudness(const kick_coefs_t *c)
{
    static kick_engine_t e;
    static float blk[256];
    float env = 0.0f, sum = 0.0f;
    uint32_t n = 0, beat = KICK_RATE * 60u / 160u, i;
    kick_engine_reset(&e);
    kick_trigger(&e, c, 0);
    while (n < beat) {
        uint32_t m = beat - n < 256u ? beat - n : 256u;
        kick_render(&e, c, blk, m);
        kick_master(&env, blk, m);
        for (i = 0; i < m; i++)
            sum += blk[i] * blk[i];
        n += m;
    }
    sum /= (float)beat;
    return sum > 1e-12f ? 3.0103f * fm_log2f(sum) : -120.0f;
}

static uint32_t dice(uint32_t *s)        /* 24 random bits */
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}
static int dice_in(uint32_t *s, int lo, int hi) { return lo + (int)(dice(s) % (uint32_t)(hi - lo + 1)); }
static int dice_log(uint32_t *s, int lo, int hi)   /* spread evenly over octaves */
{
    float u = (float)(dice(s) & 0xFFFFu) * (1.0f / 65535.0f);
    return (int)((float)lo * fm_exp2f(u * fm_log2f((float)hi / (float)lo)) + 0.5f);
}

/* an EQ: every band an extreme peak or dip, as an EQ plugin is drawn for a kick: cuts deep and narrow, boosts wide */
static void dice_eq(kick_params_t *p, int eq, int flo, int fhi, uint32_t *s)
{
    int b, base = kick_eq_param(eq);
    for (b = 0; b < kick_eq_bands(eq); b++) {
        int16_t *v = &p->v[base + KB_N * b];
        int cut = dice_in(s, 0, 1);
        v[KB_FREQ] = (int16_t)dice_log(s, flo, fhi);
        v[KB_GAIN] = (int16_t)(cut ? -dice_in(s, 15, 30) : dice_in(s, 9, 24));
        v[KB_Q] = (int16_t)(cut ? dice_log(s, 200, 2500) : dice_log(s, 50, 400));
    }
}

static void dice_dist(kick_params_t *p, int d, int drive_lo, int drive_hi, uint32_t *s)
{
    p->v[d + KD_TYPE] = (int16_t)dice_in(s, KDT_TANH, KDT_N - 1);
    p->v[d + KD_DRIVE] = (int16_t)dice_in(s, drive_lo, drive_hi);
    p->v[d + KD_BIAS] = (int16_t)(dice_in(s, 0, 2) ? 0 : dice_in(s, -10, 10) * 5);   /* a bias one time in three */
    p->v[d + KD_OUT] = 0;
    p->v[d + KD_MIX] = 100;
}

int kick_randomize(kick_params_t *p, uint32_t *seed)
{
    kick_params_t q, best;
    kick_coefs_t c;
    float best_off = 1e9f;
    int tries;
    for (tries = 1; tries <= RND_TRIES; tries++) {
        float db = -120.0f;
        int k;
        q = *p;
        q.v[KP_SWEEP] = (int16_t)(dice_in(seed, 20, 100) * 5);   /* the 909's attack: a sweep of 1..5 octaves, */
        q.v[KP_CLICK] = (int16_t)(dice_in(seed, 0, 40) * 2);     /* a click of 0..80 %, */
        q.v[KP_DECAY] = (int16_t)(dice_in(seed, 40, 180) * 5);   /* a kick's length, 200..900 ms, each as likely */
        dice_eq(&q, KE_EQ1, 30, 1000, seed);   /* before the first distortion the lows matter most */
        dice_dist(&q, KP_D1, 6, 30, seed);
        dice_eq(&q, KE_EQ2, 100, 8000, seed);  /* the overtones it made */
        dice_dist(&q, KP_D2, 0, 15, seed);     /* milder: a heavy last clip would flatten what the EQs did */
        for (k = 0; k < 3; k++) {              /* DIST2's OUT to a new kick's loudness */
            int out = q.v[KP_D2 + KD_OUT], to;
            kick_cook(&q, &c);
            db = kick_loudness(&c);
            to = out + (int)(RND_LOUD - db + (RND_LOUD > db ? 0.5f : -0.5f));
            to = to < INFO[KP_D2 + KD_OUT].min ? INFO[KP_D2 + KD_OUT].min
                 : to > INFO[KP_D2 + KD_OUT].max ? INFO[KP_D2 + KD_OUT].max : to;
            if (to == out)
                break;
            q.v[KP_D2 + KD_OUT] = (int16_t)to;
        }
        kick_cook(&q, &c);
        db = kick_loudness(&c);
        if (fm_fabsf(db - RND_LOUD) < best_off) {
            best_off = fm_fabsf(db - RND_LOUD);
            best = q;
        }
        if (best_off <= RND_SPREAD)
            break;
    }
    *p = best;
    return tries > RND_TRIES ? RND_TRIES : tries;
}
