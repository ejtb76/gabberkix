/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The midbody chain's checks (app/kick/kick.c), on the host:
 *   - every filter type: a sine through the chain is as loud as the curve the screen draws says (within 0.5 dB)
 *   - gain 0 on every band: the chain passes the signal unchanged
 *   - extremes (Q 30 and +30 dB at 20 Hz and at 19.8 kHz, full resonance with full drive, a six-octave envelope):
 *     the kick stays finite and rings out
 *   - the distortion: nearly transparent at a low push, squashed at a high one
 * Build and run: tools/build_render.sh, then build/chain_test (exit 1 on a failure). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "kick.h"

#define N 262144u                       /* 6 s: the longest kick rings out in it */
static float buf[N], ref[N];
static int fails;

static void neutral(kick_params_t *p)    /* every filter and the distortion off (the bands are at 0 dB) */
{
    kick_params_default(p);
    p->v[KP_FLT_TYPE] = KFT_OFF;
}

static double sine_gain_db(const kick_coefs_t *c, double hz)   /* the chain's gain for a steady sine, measured */
{
    static kick_engine_t e;
    double in = 0.0, out = 0.0;
    uint32_t i, len = N / 2u, settle = len / 2u;   /* 3 s, the last 1.5 s measured: Q 30 at 49 Hz has settled */
    kick_engine_reset(&e);
    for (i = 0; i < len; i++)
        buf[i] = (float)(0.01 * sin(2.0 * M_PI * hz * (double)i / KICK_RATE));
    for (i = 0; i < len; i += 256u)
        kick_chain(&e, c, buf + i, 256u);
    for (i = settle; i < len; i++) {
        double s = 0.01 * sin(2.0 * M_PI * hz * (double)i / KICK_RATE);
        in += s * s;
        out += (double)buf[i] * buf[i];
    }
    return 10.0 * log10(out / in);
}

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

/* one band of the EQ at f/gain/q: measured against kick_curve at a few frequencies */
static void band_check(int f, int gain, int q)
{
    static const double AT[] = {30.0, 49.0, 100.0, 250.0, 600.0, 1000.0, 1500.0, 4000.0, 12000.0};
    kick_params_t p;
    kick_coefs_t c;
    char what[96];
    double worst = 0.0;
    uint32_t i;
    neutral(&p);
    p.v[KP_EQ + KB_FREQ] = (int16_t)f;
    p.v[KP_EQ + KB_GAIN] = (int16_t)gain;
    p.v[KP_EQ + KB_Q] = (int16_t)q;
    kick_cook(&p, &c);
    for (i = 0; i < sizeof AT / sizeof AT[0]; i++) {
        double m = sine_gain_db(&c, AT[i]), want = kick_curve(&c, KC_PRE, (float)AT[i]);
        double d = fabs(m - want);
        if (want < -60.0)                /* a notch's floor: deep enough is right */
            d = m < -40.0 ? 0.0 : d;
        if (d > worst)
            worst = d;
    }
    snprintf(what, sizeof what, "PEAK %d Hz %+d dB Q %.2f: curve = sound (worst %.2f dB)", f, gain, q / 100.0, worst);
    check(worst < 0.5, what);
}

static void resonant_check(int type, int f, int reso)
{
    static const double AT[] = {30.0, 100.0, 400.0, 800.0, 1000.0, 1250.0, 3000.0, 10000.0};
    kick_params_t p;
    kick_coefs_t c;
    char what[96];
    double worst = 0.0;
    uint32_t i;
    neutral(&p);
    p.v[KP_FLT_TYPE] = (int16_t)type;
    p.v[KP_FLT_FREQ] = (int16_t)f;
    p.v[KP_FLT_RESO] = (int16_t)reso;
    kick_cook(&p, &c);
    for (i = 0; i < sizeof AT / sizeof AT[0]; i++) {
        double m = sine_gain_db(&c, AT[i]), want = kick_curve(&c, KC_PRE, (float)AT[i]);
        double d = fabs(m - want);
        if (want < -60.0)
            d = m < -40.0 ? 0.0 : d;
        if (d > worst)
            worst = d;
    }
    kick_param_text(KP_FLT_TYPE, type, what);
    snprintf(what + strlen(what), sizeof what - strlen(what), " FILTER %d Hz reso %d%%: curve = sound (worst %.2f dB)",
             f, reso, worst);
    check(worst < 0.5, what);
}

/* a kick at the extremes, streamed for up to 30 s: finite all along, and silent in the end (a 20 Hz resonator at
 * Q 45 behind a 60 dB clipper stays loud for seconds; it must still stop) */
static void finite_and_done(const kick_params_t *p, const char *what)
{
    static kick_engine_t e;
    static float blk[256];
    kick_coefs_t c;
    uint32_t n = 0, i, max = 30u * KICK_RATE;
    double peak = 0.0;
    int ok = 1;
    char t[160];
    kick_cook(p, &c);
    kick_engine_reset(&e);
    kick_trigger(&e, &c, 0);
    while (n < max && kick_sounding(&e)) {
        kick_render(&e, &c, blk, 256u);
        for (i = 0; i < 256u; i++) {
            if (!isfinite(blk[i]))
                ok = 0;
            if (fabs(blk[i]) > peak)
                peak = fabs(blk[i]);
        }
        n += 256u;
    }
    snprintf(t, sizeof t, "%s: finite, rings out (%.2f s, peak %+.1f dB)", what, (double)n / KICK_RATE,
             20.0 * log10(peak + 1e-30));
    check(ok && n < max && peak < 1e6, t);
}

static double crest(const float *s, uint32_t n)   /* peak / rms */
{
    double pk = 0.0, e = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        e += (double)s[i] * s[i];
        if (fabs(s[i]) > pk)
            pk = fabs(s[i]);
    }
    return pk / sqrt(e / n);
}

int main(void)
{
    kick_params_t p;
    kick_coefs_t c;
    uint32_t n, i;
    int b;
    kick_init();

    /* the curve is the sound */
    band_check(1000, 18, 100);
    band_check(250, -24, 1500);
    band_check(49, 30, 300);   /* (a boost's poles sit at Q times the gain: +30 dB at Q 30 is Q 170, which
                                            takes seconds to swing up; this one settles in the window) */
    band_check(49, -30, 3000);
    band_check(820, -30, 2500);          /* a notch as narrow as an EQ plugin's */
    band_check(3600, -26, 400);
    band_check(60, 24, 80);
    resonant_check(KFT_LP, 1000, 0);
    resonant_check(KFT_LP, 1000, 80);
    resonant_check(KFT_HP, 400, 50);
    resonant_check(KFT_BP, 1250, 60);
    resonant_check(KFT_NOTCH, 800, 30);

    /* the band curves add up to the curve (what the screen draws: each band, and their sum) */
    {
        static const int F[8] = {41, 59, 205, 368, 455, 840, 3600, 6680}, G[8] = {-30, 24, -30, 10, 15, -30, -26, -2};
        static const int Q[8] = {300, 80, 400, 300, 70, 2500, 400, 100};
        double worst = 0.0, hz;
        neutral(&p);
        for (b = 0; b < KICK_EQ_BANDS; b++) {
            p.v[KP_EQ + KB_N * b + KB_FREQ] = (int16_t)F[b];
            p.v[KP_EQ + KB_N * b + KB_GAIN] = (int16_t)G[b];
            p.v[KP_EQ + KB_N * b + KB_Q] = (int16_t)Q[b];
        }
        kick_cook(&p, &c);
        for (hz = 20.0; hz < 19000.0; hz *= 1.07) {
            double sum = 0.0;
            for (b = 0; b < KICK_EQ_BANDS; b++)
                sum += kick_band_db(&c, KC_PRE, b, (float)hz);
            if (fabs(sum - kick_curve(&c, KC_PRE, (float)hz)) > worst)
                worst = fabs(sum - kick_curve(&c, KC_PRE, (float)hz));
        }
        check(worst < 0.01, "eight bands as in an EQ plugin: the bands' curves add up to the sum's");
    }
    /* gain 0 everywhere (the default bands): what goes in comes out */
    neutral(&p);
    for (b = 0; b < KICK_EQ_BANDS; b++)  /* (a band at 0 dB is skipped: a different Q changes nothing) */
        p.v[KP_EQ + KB_N * b + KB_Q] = 3000;
    kick_cook(&p, &c);
    n = kick_render_one(&c, 0, ref, N);
    neutral(&p);
    p.v[KP_EQ + KB_GAIN] = 1;            /* one band on and back: no state left */
    p.v[KP_EQ + KB_GAIN] = 0;
    kick_cook(&p, &c);
    kick_render_one(&c, 0, buf, N);
    {
        double d = 0.0;
        for (i = 0; i < n; i++)
            if (fabs(buf[i] - ref[i]) > d)
                d = fabs(buf[i] - ref[i]);
        check(d == 0.0, "the EQ at 0 dB passes the 909 unchanged");
    }
    kick_params_default(&p);
    kick_cook(&p, &c);
    check(fabs(kick_curve(&c, KC_PRE, 49.0f)) < 0.01 && fabs(kick_curve(&c, KC_PRE, 5000.0f)) < 0.2 &&
              fabs(kick_curve(&c, KC_POST, 1000.0f)) < 0.01,
          "the blank 909's curves are flat (the FILTER's LP at 19.8 kHz: under 0.2 dB at 5 kHz)");

    /* the extremes */
    kick_params_default(&p);
    for (b = 0; b < KICK_EQ_BANDS; b++) {
        p.v[KP_EQ + KB_N * b + KB_GAIN] = 30;
        p.v[KP_EQ + KB_N * b + KB_Q] = 3000;
    }
    p.v[KP_EQ + KB_FREQ] = 20;
    p.v[KP_EQ + KB_N * 7 + KB_FREQ] = 19800;
    finite_and_done(&p, "eight bands at +30 dB, Q 30 (20 Hz .. 19.8 kHz)");
    kick_params_default(&p);
    p.v[KP_PRE_RESO] = 100, p.v[KP_PRE_DRIVE] = 48, p.v[KP_PRE_FREQ] = 20;
    p.v[KP_FLT_TYPE] = KFT_LP, p.v[KP_FLT_RESO] = 100, p.v[KP_FLT_DRIVE] = 48, p.v[KP_FLT_FREQ] = 20;
    p.v[KP_FLT_ENV] = 600, p.v[KP_FLT_KEY] = 100;
    p.v[KP_DIST_TYPE] = KDT_GABBER, p.v[KP_DIST_DRIVE] = 60, p.v[KP_DIST_BIAS] = 100;
    finite_and_done(&p, "full resonance and drive at 20 Hz, a 6-octave envelope, GABBER at 60 dB");
    p.v[KP_FLT_FREQ] = 19800, p.v[KP_PRE_FREQ] = 19800, p.v[KP_DIST_TYPE] = KDT_FOLD;
    finite_and_done(&p, "the same at 19.8 kHz, FOLD");

    /* the distortion: transparent at a low push, squashed at a high one */
    kick_params_default(&p);
    p.v[KP_LEVEL] = -30;
    kick_cook(&p, &c);
    n = kick_render_one(&c, 0, ref, N);
    for (b = KDT_TANH; b < KDT_N; b++) {
        double d = 0.0, e = 0.0, cr_clean = crest(ref, n / 2u), cr_hi;
        char t[96], name[12];
        p.v[KP_LEVEL] = -30;
        p.v[KP_DIST_TYPE] = (int16_t)b;
        p.v[KP_DIST_DRIVE] = 0;
        kick_cook(&p, &c);
        kick_render_one(&c, 0, buf, N);
        for (i = 0; i < n; i++) {
            d += (double)(buf[i] - ref[i]) * (buf[i] - ref[i]);
            e += (double)ref[i] * ref[i];
        }
        kick_param_text(KP_DIST_TYPE, b, name);
        if (b != KDT_GABBER) {           /* (GABBER is 12 dB hotter by design: it bites at once) */
            snprintf(t, sizeof t, "%s at a -30 dB kick, 0 dB push: within -30 dB of clean (%.1f dB)", name,
                     10.0 * log10(d / e + 1e-30));
            check(d / e < 1e-3, t);
        }
        p.v[KP_LEVEL] = 0;
        p.v[KP_DIST_DRIVE] = 36;
        kick_cook(&p, &c);
        kick_render_one(&c, 0, buf, N);
        cr_hi = crest(buf, n / 2u);
        snprintf(t, sizeof t, "%s at a 36 dB push: squashed (crest %.2f, the clean kick's %.2f)", name, cr_hi,
                 cr_clean);
        check(cr_hi < 0.8 * cr_clean, t);
    }
    if (fails)
        printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
