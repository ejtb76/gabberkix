/* SPDX-License-Identifier: GPL-3.0-only AND MPL-2.0
 * Copyright (C) 2026 Presethead. text_cat, int_text and app_say are from fm1-base's template (template/app.c),
 * Copyright (C) 2026 Charles Vestal, MPL-2.0 (base/LICENSE); the rest is GPL-3.0-only (LICENSE). */
/* GABBERKIX: a kick designer for the FM-1. Phase 1: the midbody's source, a synthesized 909, played by PLAY (a kick
 * on every beat at the root, G1) and by the keys and MIDI notes (pitched, an octave down to an octave up).
 *
 * The panel:
 *   SELECT       the page (one so far: 909)
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
/* a page: up to eight parameters, K1..K4 and (SEL held) K1..K4 again; -1 = none */
typedef struct {
    const char *name;
    int8_t p[8];
} page_t;
static const page_t PAGES[] = {
    {"909", {KP_SWEEP, KP_SWEEP_T, KP_HOLD, KP_DECAY, KP_TUNE, KP_CLICK, KP_LEVEL, -1}},
};
#define NPAGES ((int)(sizeof PAGES / sizeof PAGES[0]))

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
    coefs_cur = next;                    /* (the render reads coefs[coefs_cur] only at a trigger) */
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
        kick_render(&engine, mono + done, m);
        done += m;
    }
    for (i = 0; i < n; i++) {
        float s = mono[i];
        gain_now += (gain - gain_now) * 0.004f;
        if (s > 1.0f)                    /* the master: nothing above 0 dBFS reaches the output */
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
    uint32_t wave_ms;                    /* the kick's length */
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

static void param_turn(int slot, int detents)
{
    int i = PAGES[ui.page].p[slot];
    char t[12];
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
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        if (k == EN_SELECT) {
            int p = ui.page + (e > 0 ? 1 : -1);
            ui.page = p < 0 ? 0 : p >= NPAGES ? NPAGES - 1 : p;
            ui.focus = -1;
            app_say("PAGE", PAGES[ui.page].name);
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
    plat_leds((playing ? 1u << B_PLAY : 0u) | (btn & (1u << B_SEL)), keys);
}

void app_input_only(void) { input(); }

/* the picture of the kick: one kick rendered as the render would, the first second of it, a column each 1/240 s */
#define WAVE_H 80
static void wave_make(void)
{
    static kick_engine_t e;
    static float buf[256];
    uint32_t col = 0, at = 0, per = KICK_RATE / 240u, n;
    float lo = 0.0f, hi = 0.0f;
    kick_engine_reset(&e);
    kick_trigger(&e, &coefs[coefs_cur], 0);
    ui.wave_ms = 0;
    for (n = 0; n < (uint32_t)KICK_RATE * 4u && kick_sounding(&e); n += 256u) {
        uint32_t i;
        kick_render(&e, buf, 256u);
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
    ui.wave_ms = n * 1000u / KICK_RATE;
    for (; col < 240u; col++)
        ui.wave_lo[col] = ui.wave_hi[col] = WAVE_H / 2 - 1;
}

static void draw(void)
{
    char t[24];
    uint32_t i, now = plat_ms();
    int row = (ui.btn >> B_SEL) & 1u;
    const page_t *pg = &PAGES[ui.page];
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
    cv_text(8, 4, &FONT_XS, "PAGE", C_GRAY);
    cv_text(44, 4, &FONT_XS, pg->name, C_HI);
    cv_text(150, 4, &FONT_XS, row ? "SEL: ROW 2" : "ROW 1", C_DIM);
    for (i = 0; i < 8u; i++) {
        int32_t x = 4 + (int32_t)(i % 4u) * 59, y = 20 + (int32_t)(i / 4u) * 34;
        int p = pg->p[i], lit = (int)(i / 4u) == row;
        int focus = ui.focus == (int)i && now - ui.focus_at < 1500u;
        cv_rect(x, y, 56, 31, focus ? C_AMB : lit ? C_LINE : C_BLACK);
        if (p < 0)
            continue;
        cv_text(x + 4, y + 2, &FONT_XS, kick_param_info(p)->name, focus ? C_BLACK : lit ? C_GRAY : C_DIM);
        kick_param_text(p, params.v[p], t);
        cv_text(x + 4, y + 15, &FONT_XS, t, focus ? C_BLACK : lit ? C_HI : C_DIM);
    }
    cv_blit(0, 30);
    /* the kick: its first second, and its pitch */
    cv_begin(240, WAVE_H + 40, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    for (i = 1; i < 4u; i++)                     /* 250 ms marks */
        cv_rect((int32_t)i * 60, 12, 1, WAVE_H, C_LINE);
    cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_LINE);
    for (i = 0; i < 240u; i++)
        cv_rect((int32_t)i, 12 + ui.wave_hi[i], 1, ui.wave_lo[i] - ui.wave_hi[i] + 1, C_HI);
    note_text(0, t);
    cv_text(8, WAVE_H + 18, &FONT_XS, "ROOT", C_GRAY);
    cv_text(44, WAVE_H + 18, &FONT_XS, t, C_HI);
    int_text((int32_t)ui.wave_ms, t);
    text_cat(t, " MS");
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
    else if (what[0] == 'p' && what[1] >= '0' && what[1] <= '9')
        *got = params.v[atoi(what + 1) % KP_COUNT];
    else
        return 0;
    return 1;
}
#endif
