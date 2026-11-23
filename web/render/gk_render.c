/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The render page's engine (web/render/index.html): GABBERKIX's presets rendered to audio in the browser, by the kick
 * engine the firmware runs (app/kick/kick.c), compiled to WebAssembly (tools/build_web.sh). A preset sounds as it does
 * on the FM-1 when PLAY plays it: its SOURCE (another preset, resampled) and its punch's source (a preset's midbody)
 * as app.c's cook() and resample_check() make them, the whole kick, then the master's limiter (not MASTER's volume).
 *
 * The page:
 *   gk_in()            a buffer for the presets object (OBJ_PRESETS: the backup's record, app.c presets_t), up to
 *                      GK_IN_MAX bytes
 *   gk_load(len)       read it: the used slots' bits (bit s: slot s), -1 not GABBERKIX's presets. Presets from an
 *                      older firmware get the parameters they did not have, as the FM-1 does (kick_params_upgrade)
 *   gk_render(s, semis)  slot s's kick at the root + semis (PLAY's, transposed): its length in samples, 0 empty
 *   gk_out()           the samples, float, mono, 44.1 kHz
 * No libc: freestanding WebAssembly (memcpy and memset are here, for the compiler's struct copies). */
#include <stdint.h>
#include "kick.h"

#define NPRESETS 16
#define PRESET_MAGIC 0x4B42474Bu         /* "KGBK", app.c */
#define GK_IN_MAX 4096u
#define GK_OUT_MAX ((uint32_t)KICK_RATE * 8u)   /* 8 s: longer than any kick */

#ifdef __wasm__                          /* (a native build, tools/render_ref.c's, has libc's) */
void *memcpy(void *d, const void *s, unsigned long n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}
void *memset(void *d, int c, unsigned long n)
{
    uint8_t *a = d;
    while (n--)
        *a++ = (uint8_t)c;
    return d;
}
#endif

static uint8_t in[GK_IN_MAX];
static float out[GK_OUT_MAX];
static int16_t src_buf[KICK_SRC_MAX];
static kick_params_t slots[NPRESETS];
static uint32_t used;
static kick_kit_t kit;
static kick_coefs_t coefs, p_coefs;

uint8_t *gk_in(void) { return in; }
float *gk_out(void) { return out; }
int gk_rate(void) { return KICK_RATE; }

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int gk_load(uint32_t len)
{
    uint32_t magic, np, s, i;
    if (len < 8u || len > GK_IN_MAX)
        return -1;
    magic = (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
    np = (uint32_t)in[4] | (uint32_t)in[5] << 8;
    if (magic != PRESET_MAGIC || np < 1u || len != 8u + NPRESETS * np * 2u)
        return -1;
    used = (uint32_t)in[6] | (uint32_t)in[7] << 8;
    kick_init();
    for (s = 0; s < NPRESETS; s++) {
        kick_params_t d;
        kick_params_default(&d);
        for (i = 0; i < KP_COUNT && i < np; i++) {
            uint32_t at = 8u + 2u * (s * np + i);
            d.v[i] = (int16_t)(in[at] | in[at + 1] << 8);
        }
        if (np < KP_COUNT)
            kick_params_upgrade(&d, (int)np);
        for (i = 0; i < KP_COUNT; i++) {  /* each value in its range (app.c preset_params) */
            const kick_param_info_t *f = kick_param_info((int)i);
            d.v[i] = (int16_t)clamp_i(d.v[i], f->min, f->max);
        }
        slots[s] = d;
    }
    return (int)used;
}

static int slot_used(int s) { return s >= 0 && s < NPRESETS && (used >> s & 1u); }

uint32_t gk_render(int slot, int semis)
{
    kick_params_t p;
    uint32_t n = 0, i;
    float env = 0.0f;
    int src, psrc;
    if (!slot_used(slot))
        return 0;
    p = slots[slot];
    src = p.v[KP_SRC] - 1;               /* SOURCE: a preset resampled (one level: its own source the 909) */
    kick_set_source(0, 0);
    if (src >= 0 && slot_used(src))
        kick_set_source(src_buf, kick_resample(&slots[src], src_buf, KICK_SRC_MAX));
    kick_cook(&p, &coefs);
    psrc = p.v[KP_P_SRC];                /* the punch: a preset's midbody (an empty slot: this kick's own) */
    if (psrc >= KPS_P1 && slot_used(psrc - KPS_P1)) {
        kick_params_t sp = slots[psrc - KPS_P1];
        sp.v[KP_SRC] = 0;
        kick_cook(&sp, &p_coefs);
        coefs.p_src = &p_coefs;
    }
    kick_kit_reset(&kit);
    kick_kit_trigger(&kit, &coefs, semis);
    while (n < GK_OUT_MAX && kick_kit_sounding(&kit)) {
        uint32_t m = GK_OUT_MAX - n < 256u ? GK_OUT_MAX - n : 256u;
        kick_kit_render(&kit, &coefs, out + n, m);
        kick_master(&env, out + n, m);
        n += m;
    }
    while (n > 0 && out[n - 1] < 3e-5f && out[n - 1] > -3e-5f)   /* the silence at its end (below -90 dBFS) */
        n--;
    if (n > 0) {                         /* and the last 2 ms faded: no click where it stops */
        uint32_t f = KICK_RATE / 500u < n ? KICK_RATE / 500u : n;
        for (i = 0; i < f; i++)
            out[n - f + i] *= (float)(f - i) / (float)f;
    }
    kick_set_source(0, 0);
    return n;
}
