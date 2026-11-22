/* SPDX-License-Identifier: GPL-3.0-only AND MPL-2.0
 * Copyright (C) 2026 Presethead. text_cat, int_text and app_say are from fm1-base's template (template/app.c),
 * Copyright (C) 2026 Charles Vestal, MPL-2.0 (base/LICENSE); the rest is GPL-3.0-only (LICENSE). */
/* GABBERKIX: a kick designer for the FM-1. Two layers into a group: the midbody (a synthesized 909, an EQ, a
 * distortion, a second EQ, a second distortion, an EQ after them) and the punch (the hard tick at the start of a kick),
 * pressed together in group A's clipper, then a limiter (app/kick/kick.h); played by PLAY (a kick on every beat at the
 * root, G1) and by the keys and MIDI notes (pitched, an octave down to an octave up).
 *
 * The panel:
 *   SELECT       the page: 909, EQ, D1, EQ2, D2, POST (the midbody), PNCH, P.EQ (the punch), GRP.A, and round
 *   HOME         909      EDIT  EQ (again: the next band)            LFO  EQ2 (again: the next band)
 *   FX           D1 (again: D2, and back)                            GLO  POST (again: the other band)
 *   ENV          PNCH (again: P.EQ, and back)                        SEQ  GRP.A (again: its next band)
 *   K1..K4       the page's first four parameters; with SEL held, the next four
 *   SEL + white key    load the preset on that key; SAVE + white key: save the kick there. Held, SEL and
 *                SAVE light the keys that hold a preset half, the one loaded or saved last bright
 *   ARP          throw the dice: extreme curves on both EQs, new distortions, as loud as a new kick
 *   SEL + ARP    undo the last throw
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
 * that walking through them builds the kick step by step. On an EQ page K1 chooses a band (PS_BAND) and K2..K4 are
 * that band's gain, Q and frequency. Under them a picture: the kick's first second (909); an EQ's bands (each a dot,
 * its own curve faint, the sum bright) over the spectrum after the next distortion, where its sweet spots show; or
 * the spectrum after a distortion. */
enum { V_WAVE, V_CURVE, V_SPEC, V_PUNCH };   /* V_PUNCH: the first 50 ms, the punch over the whole kick */
enum { PS_NONE = -1, PS_BAND = -2, PS_FREQ = -3, PS_GAIN = -4, PS_Q = -5 };
typedef struct {
    const char *name;
    int8_t p[8];
    uint8_t view;
    int8_t eq;                           /* an EQ page's EQ, KE_*; -1 none */
    uint8_t tap;                         /* its spectrum: the chain up to here, KT_* */
    uint8_t button;                      /* its shortcut, B_*: lit while it shows */
    uint16_t amb, hi;                    /* its colours (the palette's two brightest steps): each page its own */
} page_t;
#define COLOURS(r, g, b, r2, g2, b2) RGB(r, g, b), RGB(r2, g2, b2)
#define EQ_KNOBS {PS_BAND, PS_GAIN, PS_Q, PS_FREQ, -1, -1, -1, -1}
#define DIST_KNOBS(d) {(d) + KD_TYPE, (d) + KD_DRIVE, (d) + KD_BIAS, (d) + KD_OUT, (d) + KD_MIX, -1, -1, -1}
enum { PG_909, PG_EQ, PG_D1, PG_EQ2, PG_D2, PG_POST, PG_PUNCH, PG_PEQ, PG_A };
static const page_t PAGES[] = {
    [PG_909] = {"909", {KP_SWEEP, KP_SWEEP_T, KP_HOLD, KP_DECAY, KP_TUNE, KP_CLICK, KP_LEVEL, -1}, V_WAVE, -1, KT_ALL,
                B_HOME, COLOURS(232, 76, 60, 255, 132, 112)},
    [PG_EQ] = {"EQ", EQ_KNOBS, V_CURVE, KE_EQ1, KT_D1, B_EDIT, COLOURS(60, 180, 235, 150, 225, 255)},
    [PG_D1] = {"D1", DIST_KNOBS(KP_D1), V_SPEC, -1, KT_D1, B_FX, COLOURS(214, 70, 200, 255, 140, 240)},
    [PG_EQ2] = {"EQ2", EQ_KNOBS, V_CURVE, KE_EQ2, KT_D2, B_LFO, COLOURS(110, 120, 255, 175, 185, 255)},
    [PG_D2] = {"D2", DIST_KNOBS(KP_D2), V_SPEC, -1, KT_D2, B_FX, COLOURS(232, 136, 30, 255, 184, 70)},
    [PG_POST] = {"POST", EQ_KNOBS, V_CURVE, KE_POST, KT_MID, B_GLO, COLOURS(80, 220, 120, 150, 255, 175)},
    [PG_PUNCH] = {"PNCH", {KP_P_SRC, KP_P_LEVEL, KP_P_HOLD, KP_P_DECAY, KP_P_DRIVE, KP_P_TUNE, -1, -1}, V_PUNCH, -1,
                  KT_PUNCH, B_ENV, COLOURS(225, 205, 40, 255, 240, 130)},
    [PG_PEQ] = {"P.EQ", EQ_KNOBS, V_CURVE, KE_PEQ, KT_PUNCH, B_ENV, COLOURS(40, 200, 170, 130, 245, 220)},
    [PG_A] = {"GRP.A", {PS_BAND, PS_GAIN, PS_Q, PS_FREQ, KP_A_PUSH, KP_A_CLIP, KP_A_OUT, -1}, V_CURVE, KE_AEQ, KT_ALL,
              B_SEQ, COLOURS(240, 90, 130, 255, 165, 195)},
};
#define NPAGES ((int)(sizeof PAGES / sizeof PAGES[0]))

#define SPEC_BINS 60                     /* the spectrum: a bar each 4 columns */
#define SPEC_N 4096u                     /* over the kick's first 93 ms, Hann-windowed */

/* --------------------------------------------------------------- state --- */
static kick_params_t params;             /* what the player set (the main loop's) */
static kick_params_t params_undo;        /* before the last throw of the dice (SEL + ARP) */
static uint8_t has_undo;
static uint32_t dice_seed, dice_throws;
static int dice_db;                      /* the last throw's loudness, dBFS (the host's checks) */
static kick_coefs_t coefs[2];            /* cooked from params: the main loop writes one, then flips */
static kick_coefs_t src_coefs[2];        /* the punch's source, when it is a preset (its midbody) */
static volatile uint8_t coefs_cur;
static kick_kit_t engine;                /* the audio interrupt's */

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

static int preset_params(int slot, kick_params_t *p);

static void cook(void)
{
    uint8_t next = (uint8_t)(coefs_cur ^ 1u);
    kick_params_t sp;
    int src = params.v[KP_P_SRC];
    kick_cook(&params, &coefs[next]);
    if (src >= KPS_P1 && preset_params(src - KPS_P1, &sp)) {   /* a preset as the punch: its midbody (an empty */
        kick_cook(&sp, &src_coefs[next]);                       /* slot: this kick's own) */
        coefs[next].p_src = &src_coefs[next];
    }
    coefs_cur = next;                    /* the render reads coefs[coefs_cur] at a block's start: a turn is heard at
                                            once (the main loop never runs inside the render) */
}

/* ---------------------------------------------------------------- audio --- */
static void trigger(int semis)           /* audio interrupt */
{
    kick_kit_trigger(&engine, &coefs[coefs_cur], semis);
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
        kick_kit_render(&engine, &coefs[coefs_cur], mono + done, m);
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
    kick_kit_reset(&engine);
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
    uint8_t pwave_lo[240], pwave_hi[240];   /* PNCH: the punch alone */
    uint8_t zoom10;                      /* the 909 picture's zoom, x10 (10: none) */
    uint32_t wave_ms;                    /* the kick's length; 0: longer than the picture's second */
    float spec[SPEC_BINS];               /* the kick's spectrum, dB under its loudest bin */
    int band[KE_N];                      /* the band K2..K4 turn on each EQ page */
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

/* ------------------------------------------------------------- presets --- */
/* sixteen kicks, one on each white key, in one store object (OBJ_PRESETS). A preset is the parameters' array; they
 * are only ever added at its end (kick.h), so one saved by a firmware with fewer gets the defaults for the rest, and
 * one saved by a later firmware with more loses only those. */
#define PRESET_MAGIC 0x4B42474Bu         /* "KGBK" */
#define NPRESETS 16
static const uint8_t WHITE[NPRESETS] = PLAT_WHITE_KEYS;
typedef struct {
    uint32_t magic;
    uint16_t nparams;                    /* KP_COUNT of the firmware that saved them: each slot's length */
    uint16_t used;                       /* bit s: slot s holds a kick */
    int16_t v[NPRESETS * KP_COUNT];      /* slot s at s * nparams */
} presets_t;
static presets_t presets;                /* this firmware's layout */
static int preset_cur = -1;              /* the slot loaded or saved last; -1 none */
static uint8_t preset_edited;            /* the kick has changed since */
static uint32_t preset_saves;            /* (the host's checks) */

static void presets_load(void)
{
    static uint8_t all[PLAT_STORE_MAX];   /* (an older or newer layout: read whole, each slot re-laid) */
    int n = plat_store_head(OBJ_PRESETS, &presets, 8u), s, i, np = presets.nparams;   /* the header first: a */
    if (n < 8 || presets.magic != PRESET_MAGIC || np < 1 || 8 + NPRESETS * np * 2 > (int)PLAT_STORE_MAX ||
        plat_store_size(OBJ_PRESETS) != 8 + NPRESETS * np * 2) {
        presets.magic = PRESET_MAGIC;     /* none, or not ours: sixteen empty slots */
        presets.nparams = KP_COUNT;
        presets.used = 0;
        return;
    }
    if (np == KP_COUNT) {                 /* newer firmware's presets may be longer than this one's */
        plat_store_load(OBJ_PRESETS, &presets, sizeof presets);
        return;
    }
    plat_store_load(OBJ_PRESETS, all, sizeof all);
    for (s = 0; s < NPRESETS; s++) {
        kick_params_t d;
        for (i = 0; i < KP_COUNT && i < np; i++) {
            int at = 8 + 2 * (s * np + i);
            d.v[i] = (int16_t)(all[at] | all[at + 1] << 8);
        }
        kick_params_upgrade(&d, np);     /* the parameters it did not have: so it sounds as it did */
        for (i = 0; i < KP_COUNT; i++)
            presets.v[s * KP_COUNT + i] = d.v[i];
    }
    presets.nparams = KP_COUNT;
}

static int preset_params(int slot, kick_params_t *p)   /* a slot's kick, each value in its range; 0: empty */
{
    int i;
    if (slot < 0 || slot >= NPRESETS || !(presets.used >> slot & 1u))
        return 0;
    for (i = 0; i < KP_COUNT; i++) {
        const kick_param_info_t *in = kick_param_info(i);
        p->v[i] = (int16_t)clamp(presets.v[slot * KP_COUNT + i], in->min, in->max);
    }
    return 1;
}

static void preset_load(int slot)
{
    char t[4];
    if (!(presets.used >> slot & 1u)) {
        app_say("EMPTY", "");
        return;
    }
    preset_params(slot, &params);
    preset_cur = slot;
    preset_edited = 0;
    has_undo = 0;                        /* (SEL + ARP undoes a throw, not a load) */
    cook();
    int_text(slot + 1, t);
    app_say("PRESET", t);
    ui.focus = -1;
    ui.wave_dirty = 1;
}

static void preset_save(int slot)
{
    char t[4];
    int i;
    for (i = 0; i < KP_COUNT; i++)
        presets.v[slot * KP_COUNT + i] = params.v[i];
    presets.used |= (uint16_t)(1u << slot);
    if (plat_store_save(OBJ_PRESETS, &presets, sizeof presets) != 0) {
        app_say("NOT SAVED", "FLASH");
        return;
    }
    preset_saves++;
    preset_cur = slot;
    preset_edited = 0;
    cook();                              /* (the punch may play this preset) */
    int_text(slot + 1, t);
    app_say("SAVED", t);
}

static int white_slot(uint32_t key)      /* the preset slot of a key: -1 for a black key */
{
    int s;
    for (s = 0; s < NPRESETS; s++)
        if (WHITE[s] == key)
            return s;
    return -1;
}

void app_init(void)
{
    kick_init();
    kick_params_default(&params);
    kick_kit_reset(&engine);
    cook();
    presets_load();
    cook();                              /* (a preset as the punch's source) */
    preset_cur = -1;                     /* a new kick at power-on; the presets wait on their keys */
    preset_edited = 0;
    has_undo = 0;
}

static int *band_of_page(void)           /* the band the page's knobs turn */
{
    return &ui.band[PAGES[ui.page].eq < 0 ? 0 : PAGES[ui.page].eq];
}

static int slot_param(int slot)          /* a slot's parameter: PS_BAND, -1 none, or an index into params */
{
    int p = PAGES[ui.page].p[slot];
    if (p <= PS_FREQ)                    /* PS_FREQ, PS_GAIN, PS_Q: KB_FREQ, KB_GAIN, KB_Q */
        return kick_eq_param(PAGES[ui.page].eq) + KB_N * *band_of_page() + (PS_FREQ - p);
    return p;
}

static char band_name(int eq, int b) { return (char)((eq == KE_POST ? 'A' : '1') + b); }   /* 1..4, A B */

static void band_text(char *t)
{
    t[0] = band_name(PAGES[ui.page].eq, *band_of_page());
    t[1] = 0;
}

static void band_to(int b)
{
    char t[4];
    int n = kick_eq_bands(PAGES[ui.page].eq);
    *band_of_page() = (b % n + n) % n;   /* round: from the last band to the first */
    band_text(t);
    app_say("BAND", t);
    ui.redraw = 1;
}

static void param_turn(int slot, int detents)
{
    int i = slot_param(slot);
    char t[12];
    if (i == PS_BAND) {
        int b = *band_of_page() + detents, n = kick_eq_bands(PAGES[ui.page].eq);
        band_to(b < 0 ? 0 : b >= n ? n - 1 : b);   /* K1 stops at the ends; EDIT, LFO and GLO go round */
        ui.focus = slot;
        ui.focus_at = plat_ms();
        return;
    }
    if (i < 0)
        return;
    params.v[i] = (int16_t)kick_param_step(i, params.v[i], detents);
    preset_edited = 1;
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

/* ARP: throw the dice (the 909's sweep, click and decay, the EQs, the distortions; the rest stays); SEL + ARP: back
 * to before it */
static void dice_throw(int undo)
{
    char t[12];
    if (undo) {
        if (!has_undo)
            return;
        params = params_undo;
        has_undo = 0;
        app_say("UNDO", "");
    } else {
        kick_coefs_t c;
        params_undo = params;
        has_undo = 1;
        dice_seed ^= plat_ms() * 2654435761u + ++dice_throws;
        kick_randomize(&params, &dice_seed);
        preset_edited = 1;
        kick_cook(&params, &c);
        dice_db = (int)(kick_loudness(&c) - 0.5f);
        int_text(dice_db, t);
        text_cat(t, " dB");
        app_say("RANDOM", t);
    }
    cook();
    ui.focus = -1;
    ui.wave_dirty = 1;
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), k;
    int32_t e;
    int row = (btn >> B_SEL) & 1u;
    for (k = 0; k < NKEYS; k++)                  /* key 0 is F3 (MIDI 53) */
        if ((keys >> k & 1u) && !(ui.keys >> k & 1u)) {
            int slot = white_slot(k);
            if (btn >> B_SAVE & 1u) {            /* SAVE + a white key: the kick there; SEL + one: load it */
                if (slot >= 0)
                    preset_save(slot);
            } else if (btn >> B_SEL & 1u) {
                if (slot >= 0)
                    preset_load(slot);
            } else
                key_kick(53 + (int)k - ROOT_NOTE);
        }
    if ((btn >> B_PLAY & 1u) && !(ui.btn >> B_PLAY & 1u)) {
        if (playing)
            playing = 0;
        else {
            play_start = 1;
            playing = 1;
        }
        app_say("PLAY", playing ? "ON" : "STOP");
    }
    if ((btn >> B_ARP & 1u) && !(ui.btn >> B_ARP & 1u))
        dice_throw(row);
    for (k = 0; k < NB; k++)                     /* the page shortcuts */
        if ((btn >> k & 1u) && !(ui.btn >> k & 1u)) {
            int p = -1;
            if (k == B_HOME)
                p = PG_909;
            else if (k == B_FX)                  /* D1; again: D2, and back */
                p = ui.page == PG_D1 ? PG_D2 : PG_D1;
            else if (k == B_ENV)                 /* PNCH; again: P.EQ, and back */
                p = ui.page == PG_PUNCH ? PG_PEQ : PG_PUNCH;
            else if (k == B_EDIT || k == B_LFO || k == B_GLO || k == B_SEQ) {   /* an EQ page; again: its next band */
                int to = k == B_EDIT ? PG_EQ : k == B_LFO ? PG_EQ2 : k == B_GLO ? PG_POST : PG_A;
                if (ui.page == to)
                    band_to(*band_of_page() + 1);
                else
                    p = to;
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
    {
        uint32_t lit = (playing ? 1u << B_PLAY : 0u) | (btn & (1u << B_SEL | 1u << B_SAVE)) | 1u << PAGES[ui.page].button;
        if (btn & (1u << B_SEL | 1u << B_SAVE)) {   /* SEL or SAVE held: the presets on the white keys */
            uint32_t dim = 0, bright = 0;
            int s2;
            for (s2 = 0; s2 < NPRESETS; s2++)
                if (presets.used >> s2 & 1u)
                    dim |= 1u << WHITE[s2];
            if (preset_cur >= 0)
                bright = 1u << WHITE[preset_cur];
            plat_lights(lit, bright, 0, 0, 1);
            plat_lights_mid(0, dim & ~bright);   /* the mid level (~12 % of lit): the glow's is too faint to see */
        } else {
            plat_glow(1);
            plat_leds(lit, keys);
            plat_lights_mid(0, 0);
        }
    }
}

void app_input_only(void) { input(); }

/* the picture of the kick: one kick rendered as the render would, the first second of it, a column each 1/240 s;
 * and its spectrum (a Goertzel filter at each bar's frequency, over the first SPEC_N samples) */
#define WAVE_H 80
#define COL_HZ(x) (20.0f * fm_exp2f((float)(x) * (9.9657843f / 239.0f)))   /* column x: 20 Hz .. 20 kHz */
static void wave_make(void)
{
    static kick_kit_t e;
    static float buf[256], flo[240], fhi[240];   /* each column's lowest and highest sample, before the zoom */
    float z;
    static float gk[SPEC_BINS], g1[SPEC_BINS], g2[SPEC_BINS];
    const kick_coefs_t *c = &coefs[coefs_cur];
    const page_t *pg = &PAGES[ui.page];
    int pv = pg->view == V_PUNCH, pass;
    uint32_t n = 0, b;
    int spec = pg->view != V_WAVE && !pv;
    float top = -200.0f;
    for (b = 0; b < SPEC_BINS && spec; b++) {
        gk[b] = 2.0f * fm_cosf(FM_TWO_PI * COL_HZ(b * 4u + 2u) / (float)KICK_RATE);
        g1[b] = g2[b] = 0.0f;
    }
    ui.wave_ms = 0;
    /* the picture: the first second (on PNCH the first 50 ms: the whole kick, then the punch alone over it) */
    for (pass = 0; pass < 1 + pv; pass++) {
        uint32_t col = 0, at = 0, per = pv ? KICK_RATE / 4800u : KICK_RATE / 240u, len = pv ? per * 240u : KICK_RATE;
        uint8_t *wl = pass ? ui.pwave_lo : ui.wave_lo, *wh = pass ? ui.pwave_hi : ui.wave_hi;
        float lo = 0.0f, hi = 0.0f, top = 0.0f;
        kick_kit_reset(&e);
        e.tap = pv && !pass ? KT_ALL : pg->tap;   /* the spectrum where this page listens */
        kick_kit_trigger(&e, c, 0);
        for (n = 0; n < len && kick_kit_sounding(&e); n += 256u) {
            uint32_t i;
            kick_kit_render(&e, c, buf, 256u);
            for (i = 0; spec && i < 256u && n + i < SPEC_N; i++) {   /* a punch's tick is all at the start: */
                float w = pg->tap == KT_PUNCH ? 1.0f    /* no window to fade it in */
                          : 0.5f - 0.5f * fm_cosf(FM_TWO_PI * (float)(n + i) / (float)SPEC_N);
                float x = buf[i] * w;
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
                    flo[col] = lo;
                    fhi[col] = hi;
                    if (col >= 3u && (-lo > top || hi > top))   /* the body's peak: from 12 ms, past the tick */
                        top = -lo > hi ? -lo : hi;
                    col++;
                    at = 0;
                    lo = hi = 0.0f;
                }
            }
        }
        for (; col < 240u; col++)
            flo[col] = fhi[col] = 0.0f;
        /* the 909 page zooms in so the body fills the height (the tick may run into the edge); PNCH shows its two
           layers at the same scale, unzoomed */
        z = 1.0f;
        if (!pv) {
            static const float STEPS[] = {8.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f};
            uint32_t k;
            for (k = 0; k < sizeof STEPS / sizeof STEPS[0] && top > 0.0f; k++)
                if (top * STEPS[k] <= 1.0f) {
                    z = STEPS[k];
                    break;
                }
            ui.zoom10 = (uint8_t)(z * 10.0f + 0.5f);
        }
        for (col = 0; col < 240u; col++) {
            float l = flo[col] * z, h = fhi[col] * z;
            l = l < -1.0f ? -1.0f : l;
            h = h > 1.0f ? 1.0f : h;
            wl[col] = (uint8_t)((1.0f - l) * (WAVE_H / 2 - 1) + 0.5f);
            wh[col] = (uint8_t)((1.0f - h) * (WAVE_H / 2 - 1) + 0.5f);
        }
        if (!pv && !kick_kit_sounding(&e))
            ui.wave_ms = n * 1000u / KICK_RATE;
    }
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

static void curve_draw(int what, uint16_t color)   /* an EQ's: the sum of its bands */
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
    int nb = kick_eq_bands(what), b;
    int base = kick_eq_param(what);
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
        t[0] = band_name(what, b);
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
        int32_t x = cv_text(8, 5, &FONT_B, APP_NAME, C_WHITE);
        if (preset_cur >= 0) {           /* the preset loaded or saved last; a star: changed since */
            t[0] = 'P';
            int_text(preset_cur + 1, t + 1);
            if (preset_edited)
                text_cat(t, "*");
            cv_text(x + 10, 9, &FONT_XS, t, C_GRAY);
        }
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
            x = x1 + 6;
        }
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
    case V_CURVE:                        /* the spectrum after the next distortion, faint; the bands over it */
        freq_grid();
        spec_draw(C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_DIM);   /* 0 dB */
        bands_draw(pg->eq, *band_of_page());
        curve_draw(pg->eq, C_HI);
        break;
    case V_SPEC:
        freq_grid();
        spec_draw(C_HI);
        break;
    case V_PUNCH:                        /* the first 50 ms: the whole kick faint, the punch bright over it */
        for (i = 1; i < 10u; i++)                /* 5 ms marks */
            cv_rect((int32_t)i * 24, 12, 1, WAVE_H, C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_LINE);
        for (i = 0; i < 240u; i++)
            cv_rect((int32_t)i, 12 + ui.wave_hi[i], 1, ui.wave_lo[i] - ui.wave_hi[i] + 1, C_DIM);
        for (i = 0; i < 240u; i++)
            if (ui.pwave_lo[i] != ui.pwave_hi[i] || ui.pwave_hi[i] != WAVE_H / 2 - 1)
                cv_rect((int32_t)i, 12 + ui.pwave_hi[i], 1, ui.pwave_lo[i] - ui.pwave_hi[i] + 1, C_HI);
        break;
    default:
        for (i = 1; i < 4u; i++)                 /* 250 ms marks */
            cv_rect((int32_t)i * 60, 12, 1, WAVE_H, C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_LINE);
        for (i = 0; i < 240u; i++)
            cv_rect((int32_t)i, 12 + ui.wave_hi[i], 1, ui.wave_lo[i] - ui.wave_hi[i] + 1, C_HI);
        if (ui.zoom10 > 10u) {                   /* the zoom: "X3", "X1.5" */
            t[0] = 'X';
            t[1] = (char)('0' + ui.zoom10 / 10u);
            t[2] = '.';
            t[3] = (char)('0' + ui.zoom10 % 10u);
            t[ui.zoom10 % 10u ? 4 : 2] = 0;
            cv_text(232 - text_w(&FONT_XS, t), 13, &FONT_XS, t, C_GRAY);
        }
        break;
    }
    note_text(0, t);
    cv_text(8, WAVE_H + 18, &FONT_XS, "ROOT", C_GRAY);
    cv_text(44, WAVE_H + 18, &FONT_XS, t, C_HI);
    if (pg->view == V_PUNCH) {           /* the picture's span, not the kick's length */
        t[0] = 0;
        text_cat(t, "0-50 MS");
    } else if (ui.wave_ms) {
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
    else if (!strcmp(what, "band"))      /* the band chosen on EQ, EQ2 and POST (1 is A) */
        *got = ui.band[KE_EQ1] + 1;
    else if (!strcmp(what, "band2"))
        *got = ui.band[KE_EQ2] + 1;
    else if (!strcmp(what, "preset"))    /* the slot loaded or saved last, 1..16, 0 none; saves; the used slots' bits */
        *got = preset_cur + 1;
    else if (!strcmp(what, "saves"))
        *got = (int)preset_saves;
    else if (!strcmp(what, "used"))
        *got = presets.used;
    else if (!strcmp(what, "zoom"))      /* the 909 picture's zoom, x10 */
        *got = ui.zoom10;
    else if (!strcmp(what, "throws"))    /* the dice: how often thrown; the last kick's loudness, dBFS */
        *got = (int)dice_throws;
    else if (!strcmp(what, "dice_db"))
        *got = dice_db;
    else if (!strcmp(what, "pband"))
        *got = ui.band[KE_POST] + 1;
    else if (what[0] == 'p' && what[1] >= '0' && what[1] <= '9')
        *got = params.v[atoi(what + 1) % KP_COUNT];
    else
        return 0;
    return 1;
}
#endif
