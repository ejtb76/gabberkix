/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The midbody chain's checks (app/kick/kick.c), on the host:
 *   - every EQ: a sine through the chain is as loud as the curve the screen draws says (within 0.5 dB), and its
 *     bands' curves add up to that curve
 *   - gain 0 on every band: the chain passes the signal unchanged
 *   - extremes (Q 30 and +30 dB from 20 Hz to 19.8 kHz, both distortions at 60 dB): finite, and rings out
 *   - a new kick is about as loud as the clean 909; the master limiter keeps every kick under -0.3 dBFS
 *   - the distortions: nearly transparent at a low push, squashed at a high one; the screen's taps
 *   - the dice (kick_randomize): extreme EQs, and a kick as loud as a new one, never soft or silent
 * Build and run: tools/build_render.sh, then build/chain_test (exit 1 on a failure). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kick.h"

#define N 262144u                       /* 6 s: the longest kick rings out in it */
static float buf[N], ref[N];
static int fails;

static void neutral(kick_params_t *p)    /* every EQ flat (a new kick's) and both distortions off */
{
    kick_params_default(p);
    p->v[KP_D1 + KD_TYPE] = KDT_OFF;
    p->v[KP_D2 + KD_TYPE] = KDT_OFF;
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

/* one band of an EQ at f/gain/q: measured against kick_curve at a few frequencies */
static void band_check(int eq, int f, int gain, int q)
{
    static const double AT[] = {30.0, 49.0, 100.0, 250.0, 600.0, 1000.0, 1500.0, 4000.0, 12000.0};
    kick_params_t p;
    kick_coefs_t c;
    char what[96];
    double worst = 0.0;
    uint32_t i;
    neutral(&p);
    p.v[kick_eq_param(eq) + KB_FREQ] = (int16_t)f;
    p.v[kick_eq_param(eq) + KB_GAIN] = (int16_t)gain;
    p.v[kick_eq_param(eq) + KB_Q] = (int16_t)q;
    kick_cook(&p, &c);
    for (i = 0; i < sizeof AT / sizeof AT[0]; i++) {
        double m = sine_gain_db(&c, AT[i]), want = kick_curve(&c, eq, (float)AT[i]);
        double d = fabs(m - want);
        if (want < -60.0)                /* a notch's floor: deep enough is right */
            d = m < -40.0 ? 0.0 : d;
        if (d > worst)
            worst = d;
    }
    snprintf(what, sizeof what, "%s %d Hz %+d dB Q %.2f: curve = sound (worst %.2f dB)",
             eq == KE_EQ1 ? "EQ" : eq == KE_EQ2 ? "EQ2" : "POST", f, gain, q / 100.0, worst);
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
    band_check(KE_EQ1, 1000, 18, 100);
    band_check(KE_EQ1, 250, -24, 1500);
    band_check(KE_EQ1, 49, 30, 300);     /* (a boost's poles sit at Q times the gain: +30 dB at Q 30 is Q 170, which
                                            takes seconds to swing up; this one settles in the window) */
    band_check(KE_EQ1, 49, -30, 3000);
    band_check(KE_EQ1, 820, -30, 2500);  /* a notch as narrow as an EQ plugin's */
    band_check(KE_EQ1, 3600, -26, 400);
    band_check(KE_EQ1, 60, 24, 80);
    band_check(KE_EQ2, 700, 20, 500);
    band_check(KE_POST, 120, -12, 200);

    /* the band curves add up to the curve (what the screen draws: each band, and their sum) */
    {
        static const int F[4] = {41, 59, 205, 840}, G[4] = {-30, 24, -30, -30}, Q[4] = {300, 80, 400, 2500};
        int eq;
        for (eq = 0; eq < KE_N; eq++) {
            double worst = 0.0, hz;
            char t[96];
            neutral(&p);
            for (b = 0; b < kick_eq_bands(eq); b++) {
                p.v[kick_eq_param(eq) + KB_N * b + KB_FREQ] = (int16_t)F[b];
                p.v[kick_eq_param(eq) + KB_N * b + KB_GAIN] = (int16_t)G[b];
                p.v[kick_eq_param(eq) + KB_N * b + KB_Q] = (int16_t)Q[b];
            }
            kick_cook(&p, &c);
            for (hz = 20.0; hz < 19000.0; hz *= 1.07) {
                double sum = 0.0;
                for (b = 0; b < kick_eq_bands(eq); b++)
                    sum += kick_band_db(&c, eq, b, (float)hz);
                if (fabs(sum - kick_curve(&c, eq, (float)hz)) > worst)
                    worst = fabs(sum - kick_curve(&c, eq, (float)hz));
            }
            snprintf(t, sizeof t, "%s's %d bands, extreme: their curves add up to the sum's", eq == KE_EQ1 ? "EQ"
                     : eq == KE_EQ2 ? "EQ2" : "POST", kick_eq_bands(eq));
            check(worst < 0.01, t);
        }
    }
    /* gain 0 everywhere (the default bands): what goes in comes out */
    neutral(&p);
    for (b = 0; b < KICK_EQ1_BANDS; b++) /* (a band at 0 dB is skipped: a different Q changes nothing) */
        p.v[KP_EQ1 + KB_N * b + KB_Q] = 3000;
    kick_cook(&p, &c);
    n = kick_render_one(&c, 0, ref, N);
    neutral(&p);
    p.v[KP_EQ1 + KB_GAIN] = 1;           /* one band on and back: no state left */
    p.v[KP_EQ1 + KB_GAIN] = 0;
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
    check(kick_curve(&c, KE_EQ1, 49.0f) == 0.0f && kick_curve(&c, KE_EQ2, 5000.0f) == 0.0f &&
              kick_curve(&c, KE_POST, 1000.0f) == 0.0f,
          "a new kick's EQs are flat");

    /* the extremes */
    kick_params_default(&p);
    for (b = 0; b < KICK_EQ1_BANDS; b++) {
        p.v[KP_EQ1 + KB_N * b + KB_GAIN] = 30;
        p.v[KP_EQ1 + KB_N * b + KB_Q] = 3000;
    }
    p.v[KP_EQ1 + KB_FREQ] = 20;
    p.v[KP_EQ1 + KB_N * (KICK_EQ1_BANDS - 1) + KB_FREQ] = 19800;
    finite_and_done(&p, "EQ: every band at +30 dB, Q 30 (20 Hz .. 19.8 kHz)");
    for (b = 0; b < KICK_EQ2_BANDS; b++) {
        p.v[KP_EQ2 + KB_N * b + KB_GAIN] = 30;
        p.v[KP_EQ2 + KB_N * b + KB_Q] = 3000;
    }
    p.v[KP_D1 + KD_TYPE] = KDT_GABBER, p.v[KP_D1 + KD_DRIVE] = 60, p.v[KP_D1 + KD_BIAS] = 100;
    p.v[KP_D2 + KD_TYPE] = KDT_HARD, p.v[KP_D2 + KD_DRIVE] = 60, p.v[KP_D2 + KD_BIAS] = -100;
    finite_and_done(&p, "and EQ2 the same, both distortions at 60 dB with full bias");

    /* a new kick (a soft DIST1, a GABBER DIST2) is about as loud as the clean one, and the master keeps it under
       -0.3 dBFS, pushed or not */
    {
        float env = 0.0f;
        uint32_t beat = KICK_RATE * 60u / 160u;
        double e0 = 0.0, e1 = 0.0, pk = 0.0;
        char t[128];
        neutral(&p);
        kick_cook(&p, &c);
        n = kick_render_one(&c, 0, ref, N);
        kick_params_default(&p);
        kick_cook(&p, &c);
        kick_render_one(&c, 0, buf, N);
        for (i = 0; i < beat; i++) {
            float r = ref[i] > 1.0f ? 1.0f : ref[i] < -1.0f ? -1.0f : ref[i];
            e0 += (double)r * r;
            e1 += (double)buf[i] * buf[i];
        }
        snprintf(t, sizeof t, "a new kick's loudness: %.1f dBFS a beat, the clean 909's %.1f",
                 10.0 * log10(e1 / beat), 10.0 * log10(e0 / beat));
        check(fabs(10.0 * log10(e1 / e0)) < 2.0, t);
        p.v[KP_D2 + KD_OUT] = 12;        /* 20 dB over: far over */
        p.v[KP_EQ1 + KB_N + KB_GAIN] = 30;
        kick_cook(&p, &c);
        n = kick_render_one(&c, 0, buf, N);
        for (i = 0; i < n; i += 256u)
            kick_master(&env, buf + i, n - i < 256u ? n - i : 256u);
        for (i = 0; i < n; i++)
            if (fabs(buf[i]) > pk)
                pk = fabs(buf[i]);
        snprintf(t, sizeof t, "the master limiter: a kick 20 dB over comes out at %.2f dBFS (%.7f)", 20.0 * log10(pk), pk);
        check(pk <= 0.9661, t);
    }

    /* the distortions: transparent at a low push, squashed at a high one (each alone) */
    neutral(&p);
    p.v[KP_LEVEL] = -30;
    kick_cook(&p, &c);
    n = kick_render_one(&c, 0, ref, N);
    for (b = KDT_TANH; b < KDT_N; b++) {
        int d;
        for (d = KP_D1; d <= KP_D2; d += KP_D2 - KP_D1) {
            double dd = 0.0, e = 0.0, cr_clean = crest(ref, n / 2u), cr_hi;
            char t[112], name[12];
            neutral(&p);
            p.v[KP_LEVEL] = -30;
            p.v[d + KD_TYPE] = (int16_t)b;
            p.v[d + KD_DRIVE] = 0;
            p.v[d + KD_OUT] = 0;
            kick_cook(&p, &c);
            kick_render_one(&c, 0, buf, N);
            for (i = 0; i < n; i++) {
                dd += (double)(buf[i] - ref[i]) * (buf[i] - ref[i]);
                e += (double)ref[i] * ref[i];
            }
            kick_param_text(d + KD_TYPE, b, name);
            if (b != KDT_GABBER) {       /* (GABBER is 12 dB hotter by design: it bites at once) */
                snprintf(t, sizeof t, "DIST%d %s at a -30 dB kick, 0 dB push: within -30 dB of clean (%.1f dB)",
                         d == KP_D1 ? 1 : 2, name, 10.0 * log10(dd / e + 1e-30));
                check(dd / e < 1e-3, t);
            }
            p.v[KP_LEVEL] = 0;
            p.v[d + KD_DRIVE] = 36;
            kick_cook(&p, &c);
            kick_render_one(&c, 0, buf, N);
            cr_hi = crest(buf, n / 2u);
            snprintf(t, sizeof t, "DIST%d %s at a 36 dB push: squashed (crest %.2f, the clean kick's %.2f)",
                     d == KP_D1 ? 1 : 2, name, cr_hi, cr_clean);
            check(cr_hi < 0.8 * cr_clean, t);
        }
    }

    /* the screen's taps: after DIST1 the chain stops there (EQ2, DIST2 and POST not heard) */
    {
        static kick_engine_t e;
        static float blk[256];
        double d = 0.0;
        kick_params_default(&p);
        p.v[KP_EQ2 + KB_GAIN] = 30;      /* EQ2 and DIST2 would change it a lot */
        kick_cook(&p, &c);
        kick_engine_reset(&e);
        e.tap = KT_D1;
        kick_trigger(&e, &c, 0);
        for (i = 0; i < 16384u; i += 256u) {
            kick_render(&e, &c, blk, 256u);
            memcpy(buf + i, blk, sizeof blk);
        }
        p.v[KP_EQ2 + KB_GAIN] = 0;
        p.v[KP_D2 + KD_TYPE] = KDT_OFF;
        kick_cook(&p, &c);
        kick_render_one(&c, 0, ref, 16384u);
        for (i = 0; i < 16384u; i++)
            if (fabs(buf[i] - ref[i]) > d)
                d = fabs(buf[i] - ref[i]);
        check(d == 0.0, "the tap after DIST1 is the chain up to DIST1 (EQ2 and DIST2 left out)");
    }
    /* the dice: 300 throws; every kick as loud as a new one (never soft, never silent), its EQs extreme */
    {
        uint32_t seed = 12345u;
        int k, soft = 0, loud = 0, tame = 0, throws = 0, most = 0;
        float lo = 0.0f, hi = -120.0f;
        char t[200];
        for (k = 0; k < 300; k++) {
            int j, tries;
            float db;
            kick_params_default(&p);
            tries = kick_randomize(&p, &seed);
            throws += tries;
            if (tries > most)
                most = tries;
            kick_cook(&p, &c);
            db = kick_loudness(&c);
            if (db < lo || k == 0)
                lo = db;
            if (db > hi)
                hi = db;
            if (db < -14.0f)
                soft++;
            if (db > -8.0f)
                loud++;
            for (j = 0; j < KICK_EQ1_BANDS; j++)
                if (abs(p.v[KP_EQ1 + KB_N * j + KB_GAIN]) < 9 || abs(p.v[KP_EQ2 + KB_N * j + KB_GAIN]) < 9)
                    tame++;
        }
        snprintf(t, sizeof t, "300 throws of the dice: %.1f .. %.1f dBFS a beat (a new kick: -11), none under -14 or "
                 "over -8 (%d, %d), every band 9 dB or more (%d not); %.2f throws a kick, %d at most",
                 lo, hi, soft, loud, tame, throws / 300.0, most);
        check(!soft && !loud && !tame, t);
    }
    if (fails)
        printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
