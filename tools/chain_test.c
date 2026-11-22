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

static float peak_of(const float *b, uint32_t n);

#define N 262144u                       /* 6 s: the longest kick rings out in it */
static float buf[N], ref[N];
static int fails;

static void total_neutral(kick_params_t *p)   /* no split, TOTAL a straight wire */
{
    p->v[KP_X_FREQ] = kick_param_info(KP_X_FREQ)->min;
    p->v[KP_T_CLIP] = KCL_OFF;
    p->v[KP_T_PUSH] = 0;
    p->v[KP_T_OUT] = 0;
}

static void mid_only(kick_params_t *p)   /* the midbody alone: no punch, the groups straight wires */
{
    p->v[KP_P_SRC] = KPS_OFF;
    p->v[KP_A_CLIP] = KCL_OFF;
    p->v[KP_A_PUSH] = 0;
    p->v[KP_A_OUT] = 0;
    total_neutral(p);
}

static double dft_db(const float *x, uint32_t n, double hz)   /* the whole signal's magnitude at hz (no window) */
{
    double re = 0.0, im = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        re += x[i] * cos(2.0 * M_PI * hz * i / KICK_RATE);
        im -= x[i] * sin(2.0 * M_PI * hz * i / KICK_RATE);
    }
    return 10.0 * log10(re * re + im * im + 1e-30);
}

static void neutral(kick_params_t *p)    /* every EQ flat (a new kick's), both distortions off, the midbody alone */
{
    kick_params_default(p);
    p->v[KP_D1 + KD_TYPE] = KDT_OFF;
    p->v[KP_D2 + KD_TYPE] = KDT_OFF;
    mid_only(p);
}

/* the kit's output at a tap, n samples from a kick */
static void kit_tap(const kick_coefs_t *c, int tap, float *out, uint32_t n)
{
    static kick_kit_t k;
    uint32_t i;
    kick_kit_reset(&k);
    k.tap = (uint8_t)tap;
    kick_kit_trigger(&k, c, 0);
    for (i = 0; i < n; i += 256u)
        kick_kit_render(&k, c, out + i, n - i < 256u ? n - i : 256u);
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
    static kick_kit_t e;
    static float blk[256];
    kick_coefs_t c;
    uint32_t n = 0, i, max = 30u * KICK_RATE;
    double peak = 0.0;
    int ok = 1;
    char t[160];
    kick_cook(p, &c);
    kick_kit_reset(&e);
    kick_kit_trigger(&e, &c, 0);
    while (n < max && kick_kit_sounding(&e)) {
        kick_kit_render(&e, &c, blk, 256u);
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
        mid_only(&p);
        kick_cook(&p, &c);
        kick_render_one(&c, 0, ref, 16384u);
        for (i = 0; i < 16384u; i++)
            if (fabs(buf[i] - ref[i]) > d)
                d = fabs(buf[i] - ref[i]);
        check(d == 0.0, "the tap after DIST1 is the chain up to DIST1 (EQ2 and DIST2 left out)");
    }
    /* the punch and group A */
    {
        static float mid[16384], all[16384], pun[16384];
        kick_coefs_t src;
        double d = 0.0, late = 0.0, early = 0.0, cr0, cr1;
        char t[160];
        /* a preset from before the punch, upgraded, is the midbody alone: exactly as it sounded */
        kick_params_default(&p);
        p.v[KP_EQ1 + KB_GAIN] = 18;
        kick_params_upgrade(&p, KICK_PARAMS_V1);
        kick_cook(&p, &c);
        kit_tap(&c, KT_MID, mid, 16384u);
        kit_tap(&c, KT_ALL, all, 16384u);
        for (i = 0; i < 16384u; i++)
            if (fabs(all[i] - mid[i]) > d)
                d = fabs(all[i] - mid[i]);
        check(d == 0.0, "a preset from before the punch sounds exactly as it did (no punch, no clipper)");
        /* group A at no push: a straight wire for a kick under full scale; pushed: squashed */
        neutral(&p);                     /* the clean 909, 6 dB down: under full scale */
        p.v[KP_LEVEL] = -6;
        p.v[KP_A_CLIP] = KCL_HARD;
        kick_cook(&p, &c);
        kit_tap(&c, KT_MID, mid, 16384u);
        kit_tap(&c, KT_ALL, all, 16384u);
        d = 0.0;
        for (i = 0; i < 16384u; i++)
            if (fabs(all[i] - mid[i]) > d)
                d = fabs(all[i] - mid[i]);
        snprintf(t, sizeof t, "group A, HARD at PUSH 0: the midbody (peak %.2f) passes untouched", peak_of(mid, 16384u));
        check(d == 0.0 && peak_of(mid, 16384u) < 1.0, t);
        cr0 = crest(all, 8192u);
        p.v[KP_A_PUSH] = 24;
        kick_cook(&p, &c);
        kit_tap(&c, KT_ALL, all, 16384u);
        cr1 = crest(all, 8192u);
        snprintf(t, sizeof t, "group A at PUSH 24: squashed (crest %.2f, at PUSH 0 %.2f)", cr1, cr0);
        check(cr1 < 0.8 * cr0, t);
        /* the punch: the tick alone (8 ms to -60 dB: silent after 20), loud at the start */
        kick_params_default(&p);
        kick_cook(&p, &c);
        kit_tap(&c, KT_PUNCH, pun, 16384u);
        for (i = 0; i < 16384u; i++) {
            if (i < 441u && fabs(pun[i]) > early)
                early = fabs(pun[i]);
            if (i > 882u && fabs(pun[i]) > late)
                late = fabs(pun[i]);
        }
        snprintf(t, sizeof t, "a new kick's punch: %.2f in its first 10 ms, %.1f dB under that after 20 ms", early,
                 20.0 * log10(late / early + 1e-30));
        check(early > 0.3 && late < early * 0.001, t);
        /* from a preset: another kick's tick (here one a fifth up, with no distortion) */
        neutral(&p);
        p.v[KP_TUNE] = 7;
        kick_cook(&p, &src);
        kick_params_default(&p);
        p.v[KP_P_SRC] = KPS_P1;
        kick_cook(&p, &c);
        c.p_src = &src;
        kit_tap(&c, KT_PUNCH, all, 16384u);
        d = 0.0;
        for (i = 0; i < 882u; i++)
            d += (all[i] - pun[i]) * (all[i] - pun[i]);
        snprintf(t, sizeof t, "a preset as the punch's source: another tick (%.1f dB from this kick's own)",
                 10.0 * log10(d / 882.0 + 1e-30));
        check(d / 882.0 > 1e-4, t);
    }

    /* the sub and TOTAL */
    {
        static float xa[N], xb[N], sub[16384];
        static const double AT[] = {30.0, 49.0, 80.0, 120.0, 160.0, 250.0, 500.0, 2000.0, 8000.0};
        double worst = 0.0, d = 0.0, early = 0.0, late = 0.0;
        uint32_t na, nb, j;
        char t[200];
        /* the crossover sums flat: the kick split at 120 Hz (the sub neutral, TOTAL a wire) has the spectrum of
           the kick not split (an allpass between them: the same magnitudes, the whole kick rendered) */
        kick_params_default(&p);
        total_neutral(&p);
        kick_cook(&p, &c);
        na = kick_render_one(&c, 0, xa, N);
        p.v[KP_X_FREQ] = 120;
        kick_cook(&p, &c);
        nb = kick_render_one(&c, 0, xb, N);
        for (j = 0; j < sizeof AT / sizeof AT[0]; j++) {
            double e = fabs(dft_db(xa, na, AT[j]) - dft_db(xb, nb, AT[j]));
            if (e > worst)
                worst = e;
        }
        snprintf(t, sizeof t, "the crossover at 120 Hz sums flat: the split kick's spectrum is the whole one's "
                 "(worst %.2f dB, 30 Hz .. 8 kHz)", worst);
        check(worst < 0.1, t);
        /* a preset from phase 3 (no sub parameters), upgraded: no split, TOTAL a wire: exactly as it sounded */
        kick_params_default(&p);
        kick_params_upgrade(&p, KP_X_FREQ);
        kick_cook(&p, &c);
        kit_tap(&c, KT_A, xa, 16384u);
        kit_tap(&c, KT_ALL, xb, 16384u);
        for (j = 0; j < 16384u; j++)
            if (fabs(xa[j] - xb[j]) > d)
                d = fabs(xa[j] - xb[j]);
        check(d == 0.0, "a preset from before the sub sounds exactly as it did (no split, no TOTAL clipper)");
        /* the sub's own envelope: S.DECAY 60 ms, no hold: gone after 200 ms; open: there */
        kick_params_default(&p);
        p.v[KP_S_DECAY] = 60;
        kick_cook(&p, &c);
        kit_tap(&c, KT_SUB, sub, 16384u);
        for (j = 0; j < 16384u; j++) {
            if (j < 2205u)
                early += (double)sub[j] * sub[j];
            else if (j > 8820u)
                late += (double)sub[j] * sub[j];
        }
        snprintf(t, sizeof t, "the sub's envelope at S.DECAY 60 ms: %.0f dB less after 200 ms than in the first 50",
                 10.0 * log10(early / (late + 1e-30)));
        check(early > 0.0 && late < early * 1e-4, t);
    }

    /* phase 6: noise, PERF, ENV */
    {
        static float ya[N], yb[N];
        double d = 0.0, ea = 0.0, eb = 0.0, la = 0.0, lb = 0.0;
        uint32_t na, nb, j;
        char t[200];
        /* a preset from phase 4 (none of these parameters), upgraded: exactly as it sounded */
        kick_params_default(&p);
        kick_cook(&p, &c);
        na = kick_render_one(&c, 0, ya, N);
        kick_params_upgrade(&p, KP_NOISE);
        kick_cook(&p, &c);
        nb = kick_render_one(&c, 0, yb, N);
        for (j = 0; j < na && j < nb; j++)
            if (fabs(ya[j] - yb[j]) > d)
                d = fabs(ya[j] - yb[j]);
        check(na == nb && d == 0.0, "a preset from before NOISE, PERF and ENV sounds exactly as it did");
        /* NOISE: grit in the body (the clean 909, so it shows): the high band louder */
        neutral(&p);
        kick_cook(&p, &c);
        na = kick_render_one(&c, 0, ya, N);
        p.v[KP_NOISE] = 30;
        kick_cook(&p, &c);
        nb = kick_render_one(&c, 0, yb, N);
        snprintf(t, sizeof t, "NOISE 30 %%: %.0f dB more at 5 kHz in the body", dft_db(yb, 8820u, 5000.0) - dft_db(ya, 8820u,
                 5000.0));
        check(dft_db(yb, 8820u, 5000.0) - dft_db(ya, 8820u, 5000.0) > 12.0, t);
        /* PERF's three peaks: 18 dB at 1.5 kHz (and at 300 Hz and 6 kHz), TOTAL a wire */
        kick_params_default(&p);
        mid_only(&p);
        kick_cook(&p, &c);
        na = kick_render_one(&c, 0, ya, N);
        p.v[KP_PF_FREQ] = 300;
        p.v[KP_PF_FREQ2] = 1500;
        p.v[KP_PF_FREQ3] = 6000;
        p.v[KP_PF_AMOUNT] = 18;
        kick_cook(&p, &c);
        nb = kick_render_one(&c, 0, yb, N);
        snprintf(t, sizeof t, "PERF's peaks at 18 dB, one at 1.5 kHz: %+.1f dB there",
                 dft_db(yb, 8820u, 1500.0) - dft_db(ya, 8820u, 1500.0));
        check(fabs(dft_db(yb, 8820u, 1500.0) - dft_db(ya, 8820u, 1500.0) - 18.0) < 1.0, t);
        /* PERF's LENGTH: +50 % sounds longer */
        p.v[KP_PF_AMOUNT] = 0;
        p.v[KP_PF_LENGTH] = 50;
        kick_cook(&p, &c);
        nb = kick_render_one(&c, 0, yb, N);
        snprintf(t, sizeof t, "PERF LENGTH +50 %%: the kick %u ms long, was %u", nb * 1000u / KICK_RATE,
                 na * 1000u / KICK_RATE);
        check(nb > na + na / 5u, t);
        /* ENV: the tail (from 300 ms) at -24 dB, the line gliding down to it from the middle's point (100 ms); the
           first 100 ms as they were, the gain never a step */
        kick_params_default(&p);
        kick_cook(&p, &c);
        na = kick_render_one(&c, 0, ya, N);
        p.v[KP_E_LEVEL + 3] = -24;
        kick_cook(&p, &c);
        nb = kick_render_one(&c, 0, yb, N);
        for (j = 0; j < 4410u; j++) {
            ea += (double)ya[j] * ya[j];
            eb += (double)yb[j] * yb[j];
        }
        for (j = 15435u; j < 22050u; j++) {   /* 350 .. 500 ms */
            la += (double)ya[j] * ya[j];
            lb += (double)yb[j] * yb[j];
        }
        {
            double step = 0.0;
            uint32_t k2;
            for (k2 = 1; k2 < 44100u; k2++) {
                double s = fabs(kick_env_db(&c, k2) - kick_env_db(&c, k2 - 1));
                if (s > step)
                    step = s;
            }
            snprintf(t, sizeof t, "ENV, the tail at -24 dB: %.1f dB there, %.2f dB in the first 100 ms, the line at "
                     "most %.3f dB a sample (no step)", 10.0 * log10(lb / la), 10.0 * log10(eb / ea), step);
            check(fabs(10.0 * log10(lb / la) + 24.0) < 1.0 && fabs(10.0 * log10(eb / ea)) < 0.1 && step < 0.01, t);
            snprintf(t, sizeof t, "ENV halfway from the middle to the tail (200 ms): %.1f dB (-12)",
                     kick_env_db(&c, 8820u));
            check(fabs(kick_env_db(&c, 8820u) + 12.0) < 0.2, t);
        }
    }

    /* the dice: 300 throws; every kick as loud as a new one (never soft, never silent), its EQs extreme */
    {
        uint32_t seed = 12345u;
        int k, soft = 0, loud = 0, tame = 0, throws = 0, most = 0, shorter = 0, unpunched = 0;
        long decay = 0, pdecay = 0;
        float lo = 0.0f, hi = -120.0f;
        char t[320];
        for (k = 0; k < 300; k++) {
            int j, tries;
            float db;
            kick_params_default(&p);
            p.v[KP_POST + KB_GAIN] = 12;         /* a colour from the kick before */
            p.v[KP_TEQ + KB_GAIN] = -9;
            p.v[KP_T_PUSH] = 20;
            p.v[KP_PF_AMOUNT] = 18;
            p.v[KP_PF_FREQ2] = 2500;
            tries = kick_randomize(&p, &seed, NULL);
            throws += tries;
            if (tries > most)
                most = tries;
            kick_cook(&p, &c);
            db = kick_loudness(&c);
            if (db < lo || k == 0)
                lo = db;
            if (db > hi)
                hi = db;
            decay += p.v[KP_DECAY];
            unpunched += p.v[KP_P_SRC] == KPS_OFF;
            pdecay += p.v[KP_P_DECAY];
            for (j = 0; j < KICK_EQ1_BANDS; j++) {   /* the root's body: no cut at 35..90 Hz; EQ2 not shrill */
                int f = p.v[KP_EQ1 + KB_N * j + KB_FREQ], g = p.v[KP_EQ1 + KB_N * j + KB_GAIN];
                int f2 = p.v[KP_EQ2 + KB_N * j + KB_FREQ], g2 = p.v[KP_EQ2 + KB_N * j + KB_GAIN];
                if ((g < 0 && f >= 35 && f <= 90) || (g2 > 0 && f2 > 5000) || (g2 > 12 && f2 > 2000))
                    tame++;
            }
            if (p.v[KP_X_FREQ] < 60 || p.v[KP_X_FREQ] > 200 || p.v[KP_S_GAIN] < -3 || p.v[KP_S_GAIN] > 6 ||
                p.v[KP_S_HOLD] > 100)
                tame++;                  /* (the sub out of its ranges) */
            {                            /* POST and TOTAL as on a new kick, though the kick before had them coloured */
                kick_params_t d;
                int i2;
                kick_params_default(&d);
                for (i2 = KP_POST; i2 < KP_POST + KB_N * KICK_POST_BANDS; i2++)
                    if (p.v[i2] != d.v[i2])
                        tame++;
                for (i2 = KP_TEQ; i2 <= KP_T_OUT; i2++)
                    if (p.v[i2] != d.v[i2])
                        tame++;
                for (i2 = KP_PF_FREQ; i2 <= KP_PF_LENGTH; i2++)
                    if (p.v[i2] != d.v[i2])
                        tame++;
                if (p.v[KP_PF_FREQ2] != d.v[KP_PF_FREQ2] || p.v[KP_PF_FREQ3] != d.v[KP_PF_FREQ3])
                    tame++;
            }
            if (p.v[KP_P_LEVEL] < -12 || p.v[KP_P_LEVEL] > 6 || p.v[KP_P_DECAY] < 1 || p.v[KP_P_DECAY] > 200 ||
                p.v[KP_P_HOLD] > 10 || abs(p.v[KP_P_TUNE]) > 12 || p.v[KP_P_DRIVE] != 0)
                tame++;                  /* (the punch out of its ranges, or its drive moved) */
            shorter += p.v[KP_DECAY] < 550;
            if (db < -14.0f)
                soft++;
            if (db > -8.0f)
                loud++;
            if (p.v[KP_SWEEP] < 100 || p.v[KP_SWEEP] > 500 || p.v[KP_CLICK] > 80 || p.v[KP_DECAY] < 200 ||
                p.v[KP_DECAY] > 900 || p.v[KP_TUNE] != 0 || p.v[KP_HOLD] != 20 || p.v[KP_LEVEL] != 0)
                tame++;                  /* (the 909's out of its ranges, or what stays moved) */
            for (j = 0; j < KICK_EQ1_BANDS; j++)
                if (abs(p.v[KP_EQ1 + KB_N * j + KB_GAIN]) < 9 || abs(p.v[KP_EQ2 + KB_N * j + KB_GAIN]) < 9)
                    tame++;
        }
        snprintf(t, sizeof t, "300 throws of the dice: %.1f .. %.1f dBFS a beat (a new kick: -11), none under -14 or "
                 "over -8 (%d, %d), every band 9 dB or more, the 909, the punch and the sub in their ranges, the root's body kept, EQ2 not shrill, POST, TOTAL and PERF a new kick's (%d not); %.2f throws a kick, %d at most",
                 lo, hi, soft, loud, tame, throws / 300.0, most);
        check(!soft && !loud && !tame, t);
        snprintf(t, sizeof t, "the dice's DECAY, 200..900 ms evenly: %ld ms on average (550), %d of 300 under 550 ms",
                 decay / 300, shorter);
        check(decay / 300 > 520 && decay / 300 < 580 && shorter > 125 && shorter < 175, t);
        snprintf(t, sizeof t, "the dice: no punch one time in two (%d of 300); its DECAY 1..200 ms evenly (%ld on "
                 "average)", unpunched, pdecay / 300);
        check(unpunched > 125 && unpunched < 175 && pdecay / 300 > 85 && pdecay / 300 < 115, t);
    }
    if (fails)
        printf("%d failed\n", fails);
    return fails ? 1 : 0;
}

static float peak_of(const float *b, uint32_t n)
{
    float pk = 0.0f;
    uint32_t i;
    for (i = 0; i < n; i++)
        if (fabsf(b[i]) > pk)
            pk = fabsf(b[i]);
    return pk;
}
