/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The offline render: one kick from the engine the firmware runs (app/kick/kick.c), to a WAV, measured.
 *
 *   kick_render [OUT.wav] [SEMIS] [NAME=VALUE ...] [--expect-hz HZ] [--max-ms MS]
 *
 * NAME is a parameter's name as the screen shows it (TUNE, SWEEP, ..., FREQ3, GAIN3, DRIVE1, GAIN2.2, TYPE2:
 * app/kick/kick.c), VALUE in its units (a type by its name or its number: TYPE2=HARD). It prints the kick's length,
 * its peak and the pitch of its body (its zero crossings after the sweep), and with --expect-hz fails (exit 1) when
 * that pitch is more than 1.5 % off. The WAV is 32-bit float, mono, 44.1 kHz: what the engine made, before the
 * master's limiter.
 * Build: tools/build_render.sh (gcc, in WSL or on Linux). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kick.h"

#define MAX_S (KICK_RATE * 6)
static float buf[MAX_S];

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int wav_write(const char *path, const float *s, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + n * 4u);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 3);                         /* IEEE float */
    put16(f, 1);
    put32(f, KICK_RATE);
    put32(f, KICK_RATE * 4u);
    put16(f, 4);
    put16(f, 32);
    fwrite("data", 1, 4, f);
    put32(f, n * 4u);
    fwrite(s, 4, n, f);
    return fclose(f);
}

/* the pitch of s[from .. from + len): its rising zero crossings (each placed between its two samples), counted.
 * Exact for the body of a kick, a sine whose level falls slowly; an autocorrelation reads a falling sine sharp. */
static double pitch(const float *s, uint32_t from, uint32_t len)
{
    double first = -1.0, last = 0.0;
    uint32_t i, k = 0;
    for (i = from + 1; i < from + len; i++)
        if (s[i - 1] < 0.0f && s[i] >= 0.0f) {
            double at = (double)(i - 1) + (double)s[i - 1] / ((double)s[i - 1] - (double)s[i]);
            if (first < 0.0)
                first = at;
            last = at;
            k++;
        }
    return k >= 3u ? (double)KICK_RATE * (double)(k - 1u) / (last - first) : 0.0;
}

static int param_by_name(const char *n, size_t len)
{
    int i;
    for (i = 0; i < KP_COUNT; i++)
        if (strlen(kick_param_info(i)->name) == len && !strncmp(kick_param_info(i)->name, n, len))
            return i;
    return -1;
}

static int value(int k, const char *s)   /* a number, or a type's name (F.TYPE=BP) */
{
    const kick_param_info_t *in = kick_param_info(k);
    int v;
    if (in->unit == KU_LIST)
        for (v = in->min; v <= in->max; v++)
            if (!strcmp(in->list[v - in->min], s))
                return v;
    return atoi(s);
}

int main(int argc, char **argv)
{
    kick_params_t p;
    kick_coefs_t c;
    const char *out = 0;
    int semis = 0, a, fails = 0;
    double expect = 0.0, max_ms = 4000.0, peak = 0.0, hz;
    uint32_t n, i, from, len;
    char t[16];
    kick_init();
    kick_params_default(&p);
    for (a = 1; a < argc; a++) {
        const char *eq = strchr(argv[a], '=');
        if (!strcmp(argv[a], "--expect-hz") && a + 1 < argc)
            expect = atof(argv[++a]);
        else if (!strcmp(argv[a], "--max-ms") && a + 1 < argc)
            max_ms = atof(argv[++a]);
        else if (eq) {
            int k = param_by_name(argv[a], (size_t)(eq - argv[a]));
            if (k < 0) {
                fprintf(stderr, "kick_render: no parameter %.*s\n", (int)(eq - argv[a]), argv[a]);
                return 2;
            }
            p.v[k] = (int16_t)value(k, eq + 1);
        } else if (strstr(argv[a], ".wav"))
            out = argv[a];
        else
            semis = atoi(argv[a]);
    }
    kick_cook(&p, &c);
    n = kick_render_one(&c, semis, buf, (uint32_t)(max_ms * KICK_RATE / 1000.0));
    for (i = 0; i < n; i++)
        if (fabs(buf[i]) > peak)
            peak = fabs(buf[i]);
    for (i = 0; i < KP_COUNT; i++) {     /* what differs from the blank 909 */
        if (p.v[i] == kick_param_info((int)i)->def)
            continue;
        kick_param_text((int)i, p.v[i], t);
        printf("%s %s  ", kick_param_info((int)i)->name, t);
    }
    printf("semis %+d\n", semis);
    /* the body: from eight sweep time constants (the sweep down to 1/3000) for 120 ms, or what there is */
    from = (uint32_t)(8.0 * p.v[KP_SWEEP_T] * KICK_RATE / 1000.0) + 441u;
    len = (uint32_t)(0.120 * KICK_RATE);
    if (from + len + KICK_RATE / 20 > n)
        from = n > len + KICK_RATE / 20 ? n - len - KICK_RATE / 20 : 0;
    hz = n > len + KICK_RATE / 20 ? pitch(buf, from, len) : 0.0;
    printf("length %.0f ms  peak %.2f dBFS  pitch %.2f Hz (from %.0f ms)\n", n * 1000.0 / KICK_RATE,
           peak > 0 ? 20.0 * log10(peak) : -999.0, hz, from * 1000.0 / KICK_RATE);
    if (expect > 0.0 && fabs(hz - expect) > 0.015 * expect) {
        printf("FAIL pitch %.2f Hz, expected %.2f\n", hz, expect);
        fails++;
    }
    if (out && wav_write(out, buf, n)) {
        fprintf(stderr, "kick_render: cannot write %s\n", out);
        return 2;
    }
    return fails ? 1 : 0;
}
