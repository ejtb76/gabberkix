/* SPDX-License-Identifier: GPL-3.0-only AND MPL-2.0
 * Copyright (C) 2026 Presethead. text_cat, int_text and app_say are from fm1-base's template (template/app.c),
 * Copyright (C) 2026 Charles Vestal, MPL-2.0 (base/LICENSE); the rest is GPL-3.0-only (LICENSE). */
/* GABBERKIX: a kick designer for the FM-1. The midbody so far: a synthesized 909 with a drive, an eight-band
 * parametric EQ, a resonant lowpass, a distortion and a two-band EQ after it, then a limiter (app/kick/kick.h),
 * played by PLAY (a kick on every beat at the root, G1) and by the keys and MIDI notes (pitched, an octave down to an
 * octave up).
 *
 * The panel:
 *   SELECT       the page: 909, EQ, LPF (a resonant lowpass), DIST, POST, and round
 *   HOME         909      EDIT  EQ (again: the next band)            LFO  LPF
 *   FX           DIST     GLO   POST (again: the other band)
 *   K1..K4       the page's first four parameters; with SEL held, the next four
 *   PRESETS      tempo, 60..300 BPM
 *   PLAY         a kick on every beat, or stop
 *   keys         a kick at that key's pitch: G4 (the middle G) is the root, G3 an octave down, G5 an octave up
 *   MIDI in      note on: a kick at the note's pitch, note 67 (G4) the root, folded into an octave down or up
 *   MASTER       volume
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in
 * scope. The sound is app/kick/kick.c. */
#include "fastmath.h"
#include "kick.h"

#define ROOT_NOTE 67                     /* G4: the key and the MIDI note that play the root */
#define TEMPO_MIN 60
#define TEMPO_MAX 300
#define TEMPO_DEF 160
#define BEAT_UNITS (KICK_RATE * 60u)     /* the beat clock: bpm added a frame, a beat at this many */

/* --------------------------------------------------------------- pages --- */
/* a page: up to eight parameters, K1..K4 and (SEL held) K1..K4 again; -1 = none. The pages follow the sound, so
 * that walking through them builds the kick step by step: the 909 (and its drive), the EQ, the lowpass, the
 * distortion, the EQ after it. On the EQ and POST pages K1 chooses a band (PS_BAND) and K2..K4 are that band's gain,
 * Q and frequency. Under them a picture: the kick's first second, the curves of the filters before the distortion
 * (each EQ band a dot, its own curve faint, the sum bright), the kick's spectrum, or the spectrum with the POST's
 * curves over it. */
enum { V_WAVE, V_CURVE, V_SPEC, V_POST };
enum { PS_NONE = -1, PS_BAND = -2, PS_FREQ = -3, PS_GAIN = -4, PS_Q = -5 };
typedef struct {
    const char *name;
    int8_t p[8];
    uint8_t view;
    int8_t mark[2];                      /* the frequency each row's line is at: a parameter, -1 none */
    uint8_t button;                      /* its shortcut, B_*: lit while it shows */
    uint16_t amb, hi;                    /* its colours (the palette's two brightest steps): each page its own */
} page_t;
#define COLOURS(r, g, b, r2, g2, b2) RGB(r, g, b), RGB(r2, g2, b2)
enum { PG_909, PG_EQ, PG_LPF, PG_DIST, PG_POST };
static const page_t PAGES[] = {
    [PG_909] = {"909", {KP_SWEEP, KP_SWEEP_T, KP_HOLD, KP_DECAY, KP_TUNE, KP_CLICK, KP_LEVEL, KP_DRIVE}, V_WAVE,
                {-1, -1},
                B_HOME, COLOURS(232, 76, 60, 255, 132, 112)},
    [PG_EQ] = {"EQ", {PS_BAND, PS_GAIN, PS_Q, PS_FREQ, -1, -1, -1, -1}, V_CURVE, {-1, -1}, B_EDIT,
               COLOURS(60, 180, 235, 150, 225, 255)},
    [PG_LPF] = {"LPF", {KP_LPF_FREQ, KP_LPF_RESO, -1, -1, -1, -1, -1, -1}, V_CURVE, {KP_LPF_FREQ, KP_LPF_FREQ}, B_LFO,
                COLOURS(232, 136, 30, 255, 184, 70)},
    [PG_DIST] = {"DIST", {KP_DIST_TYPE, KP_DIST_DRIVE, KP_DIST_BIAS, KP_DIST_OUT, KP_DIST_MIX, -1, -1, -1}, V_SPEC,
                 {-1, -1}, B_FX, COLOURS(214, 70, 200, 255, 140, 240)},
    [PG_POST] = {"POST", {PS_BAND, PS_GAIN, PS_Q, PS_FREQ, -1, -1, -1, -1}, V_POST, {-1, -1}, B_GLO,
                 COLOURS(80, 220, 120, 150, 255, 175)},
};
#define NPAGES ((int)(sizeof PAGES / sizeof PAGES[0]))

#define SPEC_BINS 60                     /* the spectrum: a bar each 4 columns */
#define SPEC_N 4096u                     /* over the kick's first 93 ms, Hann-windowed */

/* --------------------------------------------------------------- state --- */
static kick_params_t params;             /* what the player set (the main loop's) */
static kick_coefs_t coefs[2];            /* cooked from params: the main loop writes one, then flips */
static volatile uint8_t coefs_cur;
static kick_engine_t engine;             /* the audio interrupt's */

static volatile uint8_t playing;         /* PLAY: a kick every beat */
static volatile uint8_t play_start;      /* PLAY just pressed: the first beat now */
static volatile int32_t tempo = TEMPO_DEF;
static uint32_t beat_acc;                /* the beat clock (render) */
static volatile uint32_t kicks;          /* triggered so far (the host's checks, the screen's beat light) */
static volatile int32_t last_semis;

/* the keys' kicks, from the main loop to the render: a small ring */
#define KEYQ 8u
static volatile int8_t keyq[KEYQ];
static volatile uint32_t keyq_w, keyq_r;

static void cook(void)
{
    uint8_t next = (uint8_t)(coefs_cur ^ 1u);
    kick_cook(&params, &coefs[next]);
    coefs_cur = next;                    /* the render reads coefs[coefs_cur] at a block's start: a turn is heard at
                                            once (the main loop never runs inside the render) */
}

/* ---------------------------------------------------------------- audio --- */
static void trigger(int semis)           /* audio interrupt */
{
    kick_trigger(&engine, &coefs[coefs_cur], semis);
    last_semis = semis;
    kicks++;
}

static int fold(int semis)               /* into an octave down .. an octave up */
{
    while (semis > 12)
        semis -= 12;
    while (semis < -12)
        semis += 12;
    return semis;
}

static void midi_in(void)                /* audio interrupt: the notes that came in */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if ((st & 0xF0u) == 0x90u && d2)
            trigger(fold((int)d1 - ROOT_NOTE));
    }
}

static float mono[APP_BLOCK];
static float gain_now;                   /* glides to MASTER: no step at a block's edge */
static float lim_env;                    /* the master limiter's */

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t done = 0, i;
    float gain = (float)master_q12 * (8388607.0f / 4096.0f);
    midi_in();
    while (keyq_r != keyq_w) {
        trigger(keyq[keyq_r % KEYQ]);
        keyq_r++;
    }
    if (play_start) {
        play_start = 0;
        beat_acc = BEAT_UNITS;           /* the first beat on this block's first frame */
    }
    while (done < n) {
        uint32_t m = n - done;
        if (playing) {
            uint32_t bpm = (uint32_t)tempo, left;
            if (beat_acc >= BEAT_UNITS) {
                beat_acc -= BEAT_UNITS;
                trigger(0);
            }
            left = (BEAT_UNITS - beat_acc + bpm - 1u) / bpm;   /* frames to the next beat */
            if (left < m)
                m = left;
            beat_acc += bpm * m;
        }
        kick_render(&engine, &coefs[coefs_cur], mono + done, m);
        done += m;
    }
    kick_master(&lim_env, mono, n);      /* the master: a limiter at -0.3 dBFS, then MASTER's volume */
    for (i = 0; i < n; i++) {
        float s = mono[i];
        gain_now += (gain - gain_now) * 0.004f;
        if (s > 1.0f)                    /* (the limiter keeps it under; this is the last guard) */
            s = 1.0f;
        else if (s < -1.0f)
            s = -1.0f;
        lr[2 * i] = lr[2 * i + 1] = (int32_t)(s * gain_now);
    }
}

void app_panic(void)
{
    playing = 0;
    kick_engine_reset(&engine);
}

void app_init(void)
{
    kick_init();
    kick_params_default(&params);
    kick_engine_reset(&engine);
    cook();
}

/* ------------------------------------------------------------------- UI --- */
static struct {
    char msg[2][24];                     /* a short message over the header */
    uint32_t msg_until;
    uint32_t keys, btn;                  /* held at the last input */
    int page;
    int focus;                           /* the parameter turned last (index into the page), -1 none */
    uint32_t focus_at;
    uint8_t wave_dirty;                  /* the parameters changed: the picture of the kick again */
    uint32_t wave_at;                    /* not before (a knob turning fast redraws it now and then) */
    uint8_t wave_lo[240], wave_hi[240];  /* the picture: each column's lowest and highest sample, 0..80 */
    uint32_t wave_ms;                    /* the kick's length; 0: longer than the picture's second */
    float spec[SPEC_BINS];               /* the kick's spectrum, dB under its loudest bin */
    int band, pband;                     /* the band K2..K4 turn on the EQ page (0..7) and on POST (0..1) */
    uint32_t shown_kicks;
    uint8_t redraw;
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)     /* signed, no padding */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {                          /* and to a screen reader (announce.c, fm1-reader) */
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void note_text(int semis, char *b)    /* the kick's pitch: "G1", "A#0" */
{
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int note = 31 + params.v[KP_TUNE] + semis;   /* MIDI 31 = G1 */
    char o[8];
    b[0] = 0;
    text_cat(b, N[note % 12]);
    int_text(note / 12 - 1, o);
    text_cat(b, o);
}

static void speak(void)                      /* a screen reader that has just started listening hears the page */
{
    if (ann_on() && ann_where()) {
        char t[64];
        t[0] = 0;
        text_cat(t, APP_NAME ". PAGE ");
        text_cat(t, PAGES[ui.page].name);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static void key_kick(int semis)              /* main loop: a kick, played by the render */
{
    if (keyq_w - keyq_r < KEYQ) {
        keyq[keyq_w % KEYQ] = (int8_t)semis;
        keyq_w++;
    }
}

static int *band_of_page(void)           /* the band the page's knobs turn: the EQ's or POST's */
{
    return ui.page == PG_POST ? &ui.pband : &ui.band;
}

static int slot_param(int slot)          /* a slot's parameter: PS_BAND, -1 none, or an index into params */
{
    int p = PAGES[ui.page].p[slot];
    if (p <= PS_FREQ)                    /* PS_FREQ, PS_GAIN, PS_Q: KB_FREQ, KB_GAIN, KB_Q */
        return (ui.page == PG_POST ? KP_POST : KP_EQ) + KB_N * *band_of_page() + (PS_FREQ - p);
    return p;
}

static void band_text(char *t)           /* 1..8 on the EQ, A and B on POST */
{
    t[0] = (char)((ui.page == PG_POST ? 'A' : '1') + *band_of_page());
    t[1] = 0;
}

static void band_to(int b)
{
    char t[4];
    int n = ui.page == PG_POST ? KICK_POST_BANDS : KICK_EQ_BANDS;
    *band_of_page() = b < 0 ? 0 : b >= n ? n - 1 : b;
    band_text(t);
    app_say("BAND", t);
    ui.redraw = 1;
}

static void param_turn(int slot, int detents)
{
    int i = slot_param(slot);
    char t[12];
    if (i == PS_BAND) {
        band_to(*band_of_page() + detents);
        ui.focus = slot;
        ui.focus_at = plat_ms();
        return;
    }
    if (i < 0)
        return;
    params.v[i] = (int16_t)kick_param_step(i, params.v[i], detents);
    cook();
    kick_param_text(i, params.v[i], t);
    app_say(kick_param_info(i)->name, t);
    ui.focus = slot;
    ui.focus_at = plat_ms();
    ui.wave_dirty = 1;
}

static void page_to(int p)
{
    if (p != ui.page)
        ui.wave_dirty = 1;               /* (a page with a spectrum: it is made with the picture) */
    ui.page = p;
    ui.focus = -1;
    app_say("PAGE", PAGES[p].name);
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    int32_t e;
    int row = (btn >> B_SEL) & 1u;
    for (k = 0; k < NKEYS; k++)                  /* key 0 is F3 (MIDI 53) */
        if ((keys >> k & 1u) && !(ui.keys >> k & 1u))
            key_kick(53 + (int)k - ROOT_NOTE);
    if ((btn >> B_PLAY & 1u) && !(ui.btn >> B_PLAY & 1u)) {
        if (playing)
            playing = 0;
        else {
            play_start = 1;
            playing = 1;
        }
        app_say("PLAY", playing ? "ON" : "STOP");
    }
    for (k = 0; k < NB; k++)                     /* the page shortcuts */
        if ((btn >> k & 1u) && !(ui.btn >> k & 1u)) {
            int p = -1;
            if (k == B_HOME)
                p = PG_909;
            else if (k == B_EDIT) {
                if (ui.page == PG_EQ)            /* on the EQ page: the next band */
                    band_to((ui.band + 1) % KICK_EQ_BANDS);
                else
                    p = PG_EQ;
            }
            else if (k == B_LFO)
                p = PG_LPF;
            else if (k == B_FX)
                p = PG_DIST;
            else if (k == B_GLO) {
                if (ui.page == PG_POST)          /* on POST: the other band */
                    band_to(ui.pband ^ 1);
                else
                    p = PG_POST;
            }
            if (p >= 0)
                page_to(p);
        }
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        if (k == EN_SELECT) {
            page_to(((ui.page + (e > 0 ? 1 : -1)) % NPAGES + NPAGES) % NPAGES);   /* round: POST, 909, EQ .. */
        } else if (k == EN_PRESET) {
            char t[8];
            tempo = clamp(tempo + e, TEMPO_MIN, TEMPO_MAX);
            int_text(tempo, t);
            app_say("TEMPO", t);
        } else if (k >= EN_K1 && k <= EN_K4) {
            param_turn(row * 4 + (int)(k - EN_K1), e);
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);
    plat_leds((playing ? 1u << B_PLAY : 0u) | (btn & (1u << B_SEL)) | 1u << PAGES[ui.page].button, keys);
}

void app_input_only(void) { input(); }

/* the picture of the kick: one kick rendered as the render would, the first second of it, a column each 1/240 s;
 * and its spectrum (a Goertzel filter at each bar's frequency, over the first SPEC_N samples) */
#define WAVE_H 80
#define COL_HZ(x) (20.0f * fm_exp2f((float)(x) * (9.9657843f / 239.0f)))   /* column x: 20 Hz .. 20 kHz */
static void wave_make(void)
{
    static kick_engine_t e;
    static float buf[256];
    static float gk[SPEC_BINS], g1[SPEC_BINS], g2[SPEC_BINS];
    const kick_coefs_t *c = &coefs[coefs_cur];
    uint32_t col = 0, at = 0, per = KICK_RATE / 240u, n, b;
    int spec = PAGES[ui.page].view == V_SPEC || PAGES[ui.page].view == V_POST;
    float lo = 0.0f, hi = 0.0f, top = -200.0f;
    for (b = 0; b < SPEC_BINS && spec; b++) {
        gk[b] = 2.0f * fm_cosf(FM_TWO_PI * COL_HZ(b * 4u + 2u) / (float)KICK_RATE);
        g1[b] = g2[b] = 0.0f;
    }
    kick_engine_reset(&e);
    kick_trigger(&e, c, 0);
    ui.wave_ms = 0;
    for (n = 0; n < (uint32_t)KICK_RATE && kick_sounding(&e); n += 256u) {
        uint32_t i;
        kick_render(&e, c, buf, 256u);
        for (i = 0; spec && i < 256u && n + i < SPEC_N; i++) {
            float x = buf[i] * (0.5f - 0.5f * fm_cosf(FM_TWO_PI * (float)(n + i) / (float)SPEC_N));
            for (b = 0; b < SPEC_BINS; b++) {
                float s0 = x + gk[b] * g1[b] - g2[b];
                g2[b] = g1[b];
                g1[b] = s0;
            }
        }
        for (i = 0; i < 256u && col < 240u; i++) {
            float s = buf[i] > 1.0f ? 1.0f : buf[i] < -1.0f ? -1.0f : buf[i];
            if (s < lo)
                lo = s;
            if (s > hi)
                hi = s;
            if (++at == per) {
                ui.wave_lo[col] = (uint8_t)((1.0f - lo) * (WAVE_H / 2 - 1) + 0.5f);
                ui.wave_hi[col] = (uint8_t)((1.0f - hi) * (WAVE_H / 2 - 1) + 0.5f);
                col++;
                at = 0;
                lo = hi = 0.0f;
            }
        }
    }
    if (!kick_sounding(&e))
        ui.wave_ms = n * 1000u / KICK_RATE;
    for (; col < 240u; col++)
        ui.wave_lo[col] = ui.wave_hi[col] = WAVE_H / 2 - 1;
    for (b = 0; b < SPEC_BINS && spec; b++) {
        float pw = g1[b] * g1[b] + g2[b] * g2[b] - gk[b] * g1[b] * g2[b];
        ui.spec[b] = pw > 1e-12f ? 3.0103f * fm_log2f(pw) : -120.0f;
        if (ui.spec[b] > top)
            top = ui.spec[b];
    }
    for (b = 0; b < SPEC_BINS && spec; b++)
        ui.spec[b] -= top;
}

/* the lower picture's kinds: the waveform, a curve (dB, +-36 over the height), the spectrum's bars (60 dB) */
static int32_t db_y(float db)
{
    float y = (float)(WAVE_H / 2 - 1) - db * ((float)(WAVE_H / 2 - 1) / 36.0f);
    return y < 0.0f ? 0 : y > (float)(WAVE_H - 1) ? WAVE_H - 1 : (int32_t)(y + 0.5f);
}

static int32_t hz_x(float hz)            /* the column of a frequency */
{
    int32_t x = (int32_t)(239.0f * fm_log2f(hz / 20.0f) / 9.9657843f + 0.5f);
    return x < 0 ? 0 : x > 239 ? 239 : x;
}

static void freq_grid(void)
{
    static const float LINES[] = {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f};
    uint32_t i;
    for (i = 0; i < sizeof LINES / sizeof LINES[0]; i++)
        cv_rect(hz_x(LINES[i]), 12, 1, WAVE_H, C_LINE);
    cv_text(hz_x(100.0f) + 2, 12 + WAVE_H - 9, &FONT_XS, "100", C_DIM);
    cv_text(hz_x(1000.0f) + 2, 12 + WAVE_H - 9, &FONT_XS, "1k", C_DIM);
    cv_text(hz_x(10000.0f) + 2, 12 + WAVE_H - 9, &FONT_XS, "10k", C_DIM);
}

static void spec_draw(uint16_t color)
{
    uint32_t b;
    for (b = 0; b < SPEC_BINS; b++) {
        float db = ui.spec[b] < -60.0f ? -60.0f : ui.spec[b];
        int32_t h = (int32_t)((60.0f + db) * ((float)WAVE_H / 60.0f) + 0.5f);
        if (h > 0)
            cv_rect((int32_t)b * 4, 12 + WAVE_H - h, 3, h, color);
    }
}

static void curve_draw(int what, uint16_t color)   /* the sum: the EQ and the LPF, or POST */
{
    const kick_coefs_t *c = &coefs[coefs_cur];
    int32_t x, py = db_y(kick_curve(c, what, COL_HZ(0)));
    for (x = 0; x < 240; x++) {
        int32_t y = db_y(kick_curve(c, what, COL_HZ(x)));
        int32_t y0 = y < py ? y : py, y1 = y < py ? py : y;
        cv_rect(x, 12 + y0, 1, y1 - y0 + 1, color);
        if (x > 0 && y1 - y0 > 1)        /* steep: two pixels wide, so a narrow notch still shows */
            cv_rect(x - 1, 12 + y0, 1, y1 - y0 + 1, color);
        py = y;
    }
}

/* the bands, as an EQ plugin draws them: each its own faint curve (where it is away from 0 dB) and a dot at its
 * frequency and gain, the band the knobs turn lit and numbered */
static void bands_draw(int what, int sel)
{
    const kick_coefs_t *c = &coefs[coefs_cur];
    int nb = what == KC_POST ? KICK_POST_BANDS : KICK_EQ_BANDS, b;
    int base = what == KC_POST ? KP_POST : KP_EQ;
    int32_t x;
    for (b = 0; b < nb; b++) {
        if (!params.v[base + KB_N * b + KB_GAIN])
            continue;
        for (x = 0; x < 240; x++) {
            float db = kick_band_db(c, what, b, COL_HZ(x));
            if (db > 0.3f || db < -0.3f)
                cv_rect(x, 12 + db_y(db), 1, 1, b == sel ? C_GRAY : C_LINE);
        }
    }
    for (b = 0; b < nb; b++) {
        const int16_t *v = &params.v[base + KB_N * b];
        int32_t px = hz_x((float)v[KB_FREQ]), py = db_y((float)v[KB_GAIN]);
        char t[2];
        t[0] = what == KC_POST ? (char)('A' + b) : (char)('1' + b);
        t[1] = 0;
        if (b == sel) {
            cv_rect(px - 3, 12 + py - 3, 7, 7, C_AMB);
            cv_text(px + 5, 12 + (py > 12 ? py - 12 : py + 4), &FONT_XS, t, C_AMB);
        } else {
            cv_rect(px - 2, 12 + py - 2, 5, 5, C_GRAY);
            cv_text(px + 4, 12 + (py > 12 ? py - 11 : py + 3), &FONT_XS, t, C_DIM);
        }
    }
}

static void mark_draw(int row)           /* a line at the lit row's frequency */
{
    int p = PAGES[ui.page].mark[row];
    if (p >= 0)
        cv_rect(hz_x((float)params.v[p]), 12, 1, WAVE_H, C_AMB);
}

static void draw(void)
{
    char t[24];
    uint32_t i, now = plat_ms();
    int row = (ui.btn >> B_SEL) & 1u;
    const page_t *pg = &PAGES[ui.page];
    C_AMB = pg->amb;                     /* the page's colours */
    C_HI = pg->hi;
    /* the header: the name, the tempo and a light for each kick; or a message for a moment */
    cv_begin(240, 30, C_BLACK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(8, 5, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 5, &FONT_B, ui.msg[1], C_HI);
    } else {
        cv_text(8, 5, &FONT_B, APP_NAME, C_WHITE);
        int_text(tempo, t);
        text_cat(t, " BPM");
        cv_text(232 - text_w(&FONT_XS, t), 9, &FONT_XS, t, playing ? C_HI : C_GRAY);
    }
    cv_blit(0, 0);
    /* the page and its parameters: two rows of four, the row the knobs turn lit */
    cv_begin(240, 90, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    {                                    /* the pages, as tabs: this one lit and underlined, each in its colour */
        int32_t x = 8;
        int k;
        for (k = 0; k < NPAGES; k++) {
            int32_t x1 = cv_text(x, 4, &FONT_XS, PAGES[k].name, k == ui.page ? PAGES[k].hi : C_DIM);
            cv_rect(x - 1, 15, x1 - x + 2, k == ui.page ? 2 : 1, k == ui.page ? PAGES[k].hi : PAGES[k].amb);
            x = x1 + 10;
        }
        if (pg->p[4] >= 0)               /* a page with a second row: which one the knobs turn */
            cv_text(232 - text_w(&FONT_XS, row ? "ROW 2" : "ROW 1"), 4, &FONT_XS, row ? "ROW 2" : "ROW 1", C_DIM);
    }
    for (i = 0; i < 8u; i++) {
        int32_t x = 4 + (int32_t)(i % 4u) * 59, y = 20 + (int32_t)(i / 4u) * 34;
        int p = slot_param((int)i), lit = (int)(i / 4u) == row;
        int focus = ui.focus == (int)i && now - ui.focus_at < 1500u;
        cv_rect(x, y, 56, 31, focus ? C_AMB : lit ? C_LINE : C_BLACK);
        if (p == PS_BAND) {
            cv_text(x + 4, y + 2, &FONT_XS, "BAND", focus ? C_BLACK : lit ? C_GRAY : C_DIM);
            band_text(t);
        } else if (p < 0) {
            continue;
        } else {
            cv_text(x + 4, y + 2, &FONT_XS, kick_param_info(p)->name, focus ? C_BLACK : lit ? C_GRAY : C_DIM);
            kick_param_text(p, params.v[p], t);
        }
        cv_text(x + 4, y + 15, &FONT_XS, t, focus ? C_BLACK : lit ? C_HI : C_DIM);
    }
    cv_blit(0, 30);
    /* the kick (its first second, its spectrum) or the filters' curve; and its pitch */
    cv_begin(240, WAVE_H + 40, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    switch (pg->view) {
    case V_CURVE:
        freq_grid();
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_DIM);   /* 0 dB */
        mark_draw(row);
        bands_draw(KC_PRE, ui.page == PG_EQ ? ui.band : -1);
        curve_draw(KC_PRE, C_HI);
        break;
    case V_SPEC:
        freq_grid();
        spec_draw(C_HI);
        break;
    case V_POST:
        freq_grid();
        spec_draw(C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_DIM);
        bands_draw(KC_POST, ui.pband);
        curve_draw(KC_POST, C_HI);
        break;
    default:
        for (i = 1; i < 4u; i++)                 /* 250 ms marks */
            cv_rect((int32_t)i * 60, 12, 1, WAVE_H, C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_LINE);
        for (i = 0; i < 240u; i++)
            cv_rect((int32_t)i, 12 + ui.wave_hi[i], 1, ui.wave_lo[i] - ui.wave_hi[i] + 1, C_HI);
        break;
    }
    note_text(0, t);
    cv_text(8, WAVE_H + 18, &FONT_XS, "ROOT", C_GRAY);
    cv_text(44, WAVE_H + 18, &FONT_XS, t, C_HI);
    if (ui.wave_ms) {
        int_text((int32_t)ui.wave_ms, t);
        text_cat(t, " MS");
    } else {
        t[0] = 0;
        text_cat(t, "> 1 S");
    }
    cv_text(100, WAVE_H + 18, &FONT_XS, t, C_GRAY);
    if (kicks) {                                 /* the last kick's pitch */
        note_text(last_semis, t);
        cv_text(232 - text_w(&FONT_XS, t), WAVE_H + 18, &FONT_XS, t, C_HI);
    }
    cv_blit(0, 120);
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.focus = -1;
    ui.wave_dirty = 1;
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    input();
    speak();
    if (ui.wave_dirty && now - ui.wave_at >= 80u) {
        ui.wave_dirty = 0;
        ui.wave_at = now;
        wave_make();
        ui.redraw = 1;
    }
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.focus >= 0 && now - ui.focus_at >= 1500u) {
        ui.focus = -1;
        ui.redraw = 1;
    }
    if (kicks != ui.shown_kicks) {
        ui.shown_kicks = kicks;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "playing"))
        *got = playing;
    else if (!strcmp(what, "tempo"))
        *got = tempo;
    else if (!strcmp(what, "kicks"))
        *got = (int)kicks;
    else if (!strcmp(what, "semis"))
        *got = last_semis;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "band"))
        *got = ui.band + 1;
    else if (!strcmp(what, "pband"))     /* POST's: 1 A, 2 B */
        *got = ui.pband + 1;
    else if (what[0] == 'p' && what[1] >= '0' && what[1] <= '9')
        *got = params.v[atoi(what + 1) % KP_COUNT];
    else
        return 0;
    return 1;
}
#endif
