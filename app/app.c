/* SPDX-License-Identifier: GPL-3.0-only AND MPL-2.0
 * Copyright (C) 2026 Presethead. text_cat, int_text and app_say are from fm1-base's template (template/app.c),
 * Copyright (C) 2026 Charles Vestal, MPL-2.0 (base/LICENSE); the rest is GPL-3.0-only (LICENSE). */
/* GABBERKIX: a kick designer for the FM-1. Three layers: the midbody (a synthesized 909, an EQ, a distortion, a
 * second EQ, a second distortion, an EQ after them) and the punch (the hard tick at the start of a kick), pressed
 * together in group A's clipper; the sub, group A's lows split off; both into TOTAL's clipper, then a limiter
 * (app/kick/kick.h); played by PLAY (a kick on every beat at the
 * root, G1) and by the keys and MIDI notes (pitched, an octave down to an octave up).
 *
 * The panel:
 *   SELECT       the page: 909, EQ, D1, EQ2, D2, POST (the midbody), PNCH, P.EQ (the punch), GRP.A, and round
 *   HOME         909 (again: SRC, and back)          EDIT  EQ (again: the next band)
 *   LFO          EQ2 (again: the next band)
 *   FX           D1 (again: D2, and back)                            GLO  POST (again: the other band)
 *   ENV          PNCH (again: P.EQ, and back)                        SEQ  GRP.A (again: its next band)
 *   OCT-         SUB                                                 OCT+ TOTAL (again: its next band)
 *   REC          PERF (again: ENV, and back); SEL + REC: the connections page (Bluetooth MIDI devices, the USB
 *                port's role): SELECT moves its cursor, SAVE connects or lets go, SEL + REC back
 *   K1..K4       the page's first four parameters; with SEL held, the next four
 *   SEL + white key    load the preset on that key; SAVE + white key: save the kick there. Held, SEL and
 *                SAVE light the keys that hold a preset half, the one loaded or saved last bright
 *   ARP          throw the dice: extreme curves on both EQs, new distortions, as loud as a new kick
 *   SEL + ARP    undo the last throw
 *   PRESETS      tempo, 60..300 BPM; ALGO: TRANSPOSE, PLAY's kick and the keys -12 .. +12 semitones
 *   MIDI         clock in (USB, TRS, Bluetooth: followed, as FMLarynx and Amenity follow it) and out (PLAY on our
 *                own tempo), Start / Continue / Stop, notes in; everything passed on (THRU)
 *   PLAY         a kick on every beat, or stop
 *   keys         a kick at that key's pitch: G4 (the middle G) is the root, G3 an octave down, G5 an octave up
 *   MIDI in      note on: a kick at the note's pitch, note 67 (G4) the root, folded into an octave down or up
 *   MASTER       volume
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in
 * scope. The sound is app/kick/kick.c. */
#include "fastmath.h"
#include "kick.h"
#include "gk_clock.h"                    /* MIDI clock in: the delay-locked loop, as FMLarynx's and Amenity's */
#include "btdev.h"                       /* Bluetooth MIDI devices: the connections page (SEL + REC) */

#define ROOT_NOTE 67                     /* G4: the key and the MIDI note that play the root */
#define TEMPO_MIN 60
#define TEMPO_MAX 300
#define TEMPO_DEF 160

/* --------------------------------------------------------------- pages --- */
/* a page: up to eight parameters, K1..K4 and (SEL held) K1..K4 again; -1 = none. The pages follow the sound, so
 * that walking through them builds the kick step by step. On an EQ page K1 chooses a band (PS_BAND) and K2..K4 are
 * that band's gain, Q and frequency. Under them a picture: the kick's first second (909); an EQ's bands (each a dot,
 * its own curve faint, the sum bright) over the spectrum after the next distortion, where its sweet spots show; or
 * the spectrum after a distortion. */
/* V_PUNCH: the first 50 ms, the punch over the whole kick; V_XOVER: the crossover's two halves over group A's
 * spectrum; V_ENV: the kick, ENV's line over it; V_PERF: PERF's three peaks over the kick's spectrum */
enum { V_WAVE, V_CURVE, V_SPEC, V_PUNCH, V_XOVER, V_ENV, V_PERF };
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
enum { PG_SRC, PG_909, PG_EQ, PG_D1, PG_EQ2, PG_D2, PG_POST, PG_PUNCH, PG_PEQ, PG_A, PG_SUB, PG_TOT, PG_ENV, PG_PERF };
/* the layers, for the tab bar: the midbody's pages, the punch's, group A, the sub, TOTAL */
enum { L_MID, L_PUNCH, L_A, L_SUB, L_TOT, L_PERF, L_N };
static const uint8_t PAGE_LAYER[] = {L_MID, L_MID, L_MID, L_MID, L_MID, L_MID, L_MID, L_PUNCH, L_PUNCH, L_A, L_SUB,
                                     L_TOT, L_TOT, L_PERF};
static const char *const LAYER_NAME[L_N] = {"MID", "PN", "A", "SUB", "TOT", "PERF"};
static const char *const LAYER_SHORT[L_N] = {"M", "P", "A", "S", "T", "F"};   /* (when the line is full) */
static const page_t PAGES[] = {
    [PG_SRC] = {"SRC", {KP_SRC, KP_SRC_PITCH, KP_SRC_START, KP_SRC_LEVEL, -1, -1, -1, -1}, V_WAVE, -1, KT_ALL, B_HOME,
                COLOURS(150, 150, 160, 220, 220, 230)},
    [PG_909] = {"909", {KP_SWEEP, KP_SWEEP_T, KP_HOLD, KP_DECAY, KP_TUNE, KP_CLICK, KP_LEVEL, KP_NOISE}, V_WAVE, -1,
                KT_ALL,
                B_HOME, COLOURS(232, 76, 60, 255, 132, 112)},
    [PG_EQ] = {"EQ", EQ_KNOBS, V_CURVE, KE_EQ1, KT_D1, B_EDIT, COLOURS(60, 180, 235, 150, 225, 255)},
    [PG_D1] = {"D1", DIST_KNOBS(KP_D1), V_SPEC, -1, KT_D1, B_FX, COLOURS(214, 70, 200, 255, 140, 240)},
    [PG_EQ2] = {"EQ2", EQ_KNOBS, V_CURVE, KE_EQ2, KT_D2, B_LFO, COLOURS(110, 120, 255, 175, 185, 255)},
    [PG_D2] = {"D2", DIST_KNOBS(KP_D2), V_SPEC, -1, KT_D2, B_FX, COLOURS(232, 136, 30, 255, 184, 70)},
    [PG_POST] = {"POST", EQ_KNOBS, V_CURVE, KE_POST, KT_MID, B_GLO, COLOURS(80, 220, 120, 150, 255, 175)},
    [PG_PUNCH] = {"PNCH", {KP_P_SRC, KP_P_LEVEL, KP_P_HOLD, KP_P_DECAY, KP_P_DRIVE, KP_P_TUNE, -1, -1}, V_PUNCH, -1,
                  KT_PUNCH, B_ENV, COLOURS(225, 205, 40, 255, 240, 130)},
    [PG_PEQ] = {"P.EQ", EQ_KNOBS, V_CURVE, KE_PEQ, KT_PUNCH, B_ENV, COLOURS(40, 200, 170, 130, 245, 220)},
    [PG_A] = {"GRP.A", {PS_BAND, PS_GAIN, PS_Q, PS_FREQ, KP_A_PUSH, KP_A_CLIP, KP_A_OUT, -1}, V_CURVE, KE_AEQ, KT_A,
              B_SEQ, COLOURS(240, 90, 130, 255, 165, 195)},
    [PG_SUB] = {"SUB", {KP_X_FREQ, KP_S_GAIN, KP_S_HOLD, KP_S_DECAY, KP_S_DRIVE, -1, -1, -1}, V_XOVER, -1, KT_A,
                B_OCTDN, COLOURS(150, 90, 230, 200, 165, 255)},
    [PG_TOT] = {"TOTAL", {PS_BAND, PS_GAIN, PS_Q, PS_FREQ, KP_T_PUSH, KP_T_CLIP, KP_T_OUT, -1}, V_CURVE, KE_TEQ, KT_ALL,
                B_OCTUP, COLOURS(235, 235, 235, 255, 255, 255)},
    [PG_ENV] = {"ENV", {KP_E_LEVEL, KP_E_LEVEL + 1, KP_E_LEVEL + 2, KP_E_LEVEL + 3, KP_E_TIME, KP_E_TIME + 1,
                KP_E_TIME + 2, -1}, V_ENV, -1, KT_ALL, B_REC, COLOURS(160, 220, 40, 205, 250, 120)},
    [PG_PERF] = {"PERF", {KP_PF_AMOUNT, KP_PF_FREQ, KP_PF_FREQ2, KP_PF_FREQ3, KP_PF_DRIVE, KP_PF_LENGTH, -1, -1},
                 V_PERF,
                 -1, KT_ALL, B_REC,
                 COLOURS(255, 70, 20, 255, 150, 100)},
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
static int16_t resample_buf[KICK_SRC_MAX] PLAT_POOL;   /* RESAMPLE: the source preset's kick, rendered (88 KB) */
static int resample_slot = -1;           /* the slot in it; -1 none */
static uint32_t resample_saves = ~0u;    /* the saves when it was rendered (a save may change that slot) */
static uint32_t resample_at;             /* SOURCE last turned: rendered once the knob rests */
static volatile uint8_t muted;           /* the render silent (while the resample is rendered)  */
static volatile uint8_t coefs_cur;
static kick_kit_t engine;                /* the audio interrupt's */

static volatile uint8_t playing;         /* a kick every beat (PLAY, or an outside Start) */
static volatile int32_t tempo = TEMPO_DEF;
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
        sp.v[KP_SRC] = 0;                                       /* slot: this kick's own; its own source the 909) */
        kick_cook(&sp, &src_coefs[next]);
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

/* ---- MIDI: in, the clock, thru (the 1 kHz tick), as FMLarynx and Amenity have them.
 * The beat runs in the audio interrupt as 24 pulses a quarter note, each on its own sample (app_render renders up to
 * it, plays it, renders on), at the time it is heard: a block is heard HEARD_TICKS after it is rendered. A kick on
 * every 24th, the first at once.
 *   Ours: counted in frames at the tempo; it sends Start, the pulses and Stop to USB and Bluetooth, each at the time
 *         it is heard (out_at, sent by the tick), so another FM-1 or a DAW follows the sound, not the render.
 *   From outside (MIDI clock over USB, the TRS jack or Bluetooth): the tick reads each pulse with the time it came in
 *         (plat_midi_in_t24: Bluetooth's at its sender's time) and gk_clock.c's delay-locked loop turns those times
 *         into a smooth time line; the pulses are played on that line, never at the moment one is read, so USB's
 *         frames, a DAW's buffers, Bluetooth's bunching and the screen's drawing do not reach the beat. A Start arms,
 *         the first pulse after it is beat 1; Continue goes on where it was; Stop stops; half a second without
 *         pulses: ours plays on at the tempo it had. More than LOCK_FAR pulses behind the line (a held-up interrupt),
 *         the pulses in between pass silently, not played at once. */
#define HEARD_TICKS ((uint32_t)((uint64_t)APP_BLOCK * PLAT_TICKS_PER_US * 1000000u / 44100u))
#define TICKS_PER_FRAME ((float)GK_TICK_HZ / 44100.0f)
#define INT_PULSE 26460000u                     /* ours: a pulse is 44100 x 60 x 10 (frames x BPM x 10 x 24) */
#define MIDI_TO (TO_USB | TO_BLE)               /* out: USB (or the device it hosts) and Bluetooth */
enum { LOCK_FAR = 6, MAX_PULSES = 48 };         /* (MAX_PULSES: a block plays at most these) */
enum { RQ_NONE, RQ_START, RQ_STOP };

/* what the audio interrupt sends (the clock) goes with the time it is heard: the tick sends each then */
enum { OUTQ = 64 };
static struct { uint32_t at, pkt; } outq[OUTQ];
static volatile uint32_t outq_w, outq_r;
static void out_at(uint32_t pkt, uint32_t at)   /* audio interrupt */
{
    uint32_t w = outq_w;
    if (w - outq_r >= OUTQ)
        return;
    outq[w % OUTQ].at = at;
    outq[w % OUTQ].pkt = pkt;
    __asm__ volatile("" ::: "memory");
    outq_w = w + 1u;
}
static volatile uint32_t clocks_out;            /* pulses sent (the host's checks) */
static void clock_out(uint32_t status, uint32_t at)   /* CIN F: one byte */
{
    out_at(0x0Fu | status << 8, at);
    if (status == 0xF8u)
        clocks_out++;
}

/* the MIDI notes, from the tick to the render: a small ring of kicks (semitones from the root) */
#define MIDIQ 16u
static volatile int8_t midiq[MIDIQ];
static volatile uint32_t midiq_w, midiq_r;

static volatile uint8_t play_req;               /* main loop asks the audio interrupt: PLAY's Start or Stop */
static volatile uint8_t clock_ext;              /* tick: an outside clock is followed (gk_clock) */
static volatile uint16_t clock_bpm_x10;         /* tick: its tempo as the loop found it (BPM x 10) */
static uint32_t pulse_n;                        /* audio interrupt: pulses since the last beat */
static uint8_t beat_first;                      /* the next pulse is beat 1 */
static uint32_t int_acc;                        /* ours: frames x BPM x 10 x 24 into the pulse */
static volatile uint32_t int_hold10;            /* ours plays on at an outside clock's tempo (BPM x 10; 0: tempo) .. */
static int32_t int_hold_bpm;                    /* .. until the tempo moves from this, or PLAY / Stop / a Start */
static uint32_t ext_k;                          /* outside: the next pulse to play, counted from the Start's first */
static uint8_t int_run, seen_start, was_state;  /* ours runs (and sends); the last Start seen; the loop's last state */
static uint8_t ext_ran;                         /* the outside clock has run since its Start: only then does ours play
                                                  * on when it goes */
static volatile int8_t xpose;                   /* ALGO: PLAY and the keys moved by this semitones; not MIDI's notes */

unsigned gk_tempo_x10(void) { return (unsigned)tempo * 10u; }

static uint32_t now24(void)
{
#ifdef PLAT_HOST
    return plat_ms() * 24000u;                  /* (the simulator's plat_ticks24 is real time) */
#else
    return plat_ticks24();
#endif
}

static void on_pulse(int sound)                 /* audio interrupt: a pulse; a kick on beat 1 and every 24th after */
{
    if (!playing)
        return;
    if (beat_first) {
        beat_first = 0;
        pulse_n = 0;
    } else if (++pulse_n < 24u)
        return;
    else
        pulse_n = 0;
    if (sound)
        trigger(xpose);
}

static float mono[APP_BLOCK];
static float gain_now;                   /* glides to MASTER: no step at a block's edge */
static float lim_env;                    /* the master limiter's */
static volatile float meter_peak;        /* the output's peak since the screen read it (the meter) */

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t done = 0, i, fired = 0, now = now24() + HEARD_TICKS;   /* when this block's first frame is heard */
    float gain = (float)master_q12 * (8388607.0f / 4096.0f);
    struct gk_follow f;
    int follow;
    uint8_t req;
    if (muted) {                         /* (a resample being rendered) */
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    while (keyq_r != keyq_w) {           /* the keys' kicks and MIDI's */
        trigger(keyq[keyq_r % KEYQ]);
        keyq_r++;
    }
    while (midiq_r != midiq_w) {
        trigger(midiq[midiq_r % MIDIQ]);
        midiq_r++;
    }
    gk_follow_read(&f);
    follow = f.state == CLOCK_ARMED || f.state == CLOCK_RUNNING;
    if (f.state != CLOCK_OFF && f.start != seen_start) {   /* a Start or Continue from outside */
        seen_start = f.start;
        if (int_run)
            clock_out(0xFCu, now);       /* ours gives way */
        int_run = 0;
        ext_k = 0;
        ext_ran = 0;
        if (!f.resume)                   /* Start: beat 1 at the first pulse */
            beat_first = 1;
        playing = 1;
    }
    if (f.state == CLOCK_STOPPED && was_state != CLOCK_STOPPED && playing)   /* Stop from outside */
        playing = 0;
    was_state = f.state;
    req = play_req;
    if (req) {
        play_req = RQ_NONE;
        if (req == RQ_START) {           /* PLAY: beat 1, on ours unless one comes from outside */
            playing = 1;
            ext_ran = 1;                 /* (asked to play: ours plays on if an outside one never comes) */
            beat_first = 1;
            if (!follow) {
                int_run = 1;
                int_acc = INT_PULSE;     /* Start, and its first pulse at once: beat 1 */
                clock_out(0xFAu, now);
            }
        } else {
            if (int_run)
                clock_out(0xFCu, now);
            int_run = 0;
            playing = 0;
        }
        int_hold10 = 0;
    }
    if (f.state == CLOCK_RUNNING)
        ext_ran = 1;
    if (!playing || follow) {
        int_run = 0;
        int_hold10 = 0;
    } else if (!int_run && !ext_ran && seen_start) {   /* a Start no pulse ever followed (a computer opening the
                                                          * port): nothing to play on from */
        playing = 0;
    } else if (!int_run) {               /* the outside clock went (no Stop): ours plays on at the tempo it had */
        uint32_t t10 = f.per > 0.0f ? (uint32_t)(6.0e8f / f.per + 0.5f) : 0u;
        int_run = 1;
        int_acc = 0;
        int_hold10 = t10 >= 200u && t10 <= 6000u ? t10 : 0u;
        int_hold_bpm = tempo;
    }
    if (int_hold10 && tempo != int_hold_bpm)
        int_hold10 = 0;                  /* the tempo turned meanwhile: that, as asked */

    while (done < n) {                   /* render up to each pulse, play it, render on */
        uint32_t at = n;
        if (int_run) {
            uint32_t t10 = int_hold10 ? int_hold10 : (uint32_t)tempo * 10u;
            uint32_t inc = t10 * 24u;
            uint32_t st = int_acc >= INT_PULSE ? 0u : (INT_PULSE - int_acc + inc - 1u) / inc;
            if (done + st < n)
                at = done + st;
            int_acc += (at - done) * inc;
        } else if (playing && f.state == CLOCK_RUNNING && fired < MAX_PULSES) {
            /* pulse ext_k on the loop's line: f.pulse at t0 + t0_frac, then f.per apart */
            float d = (float)(int32_t)(f.t0 - now) + f.t0_frac + (float)(int32_t)(ext_k - f.pulse) * f.per;
            if (d < -(float)LOCK_FAR * f.per) {   /* far behind: skip to the line, the pulses passing silently */
                uint32_t k = f.pulse + (uint32_t)(int32_t)(((float)(int32_t)(now - f.t0) - f.t0_frac) * f.inv_per);
                while ((int32_t)(k - ext_k) > 0) {
                    ext_k++;
                    on_pulse(0);
                }
                continue;
            }
            d *= 1.0f / TICKS_PER_FRAME;  /* in frames from the block's start */
            if (d < (float)n)
                at = d <= (float)done ? done : (uint32_t)(d + 0.5f);
            if (at > n)
                at = n;
        }
        if (at > done) {
            kick_kit_render(&engine, &coefs[coefs_cur], mono + done, at - done);
            done = at;
        }
        if (at < n) {                    /* a pulse here */
            uint32_t heard = now + (uint32_t)((float)at * TICKS_PER_FRAME);
            if (int_run) {
                int_acc -= INT_PULSE;
                clock_out(0xF8u, heard);
            } else
                ext_k++;
            on_pulse(1);
            fired++;
        }
    }
    kick_master(&lim_env, mono, n);      /* the master: a limiter at -0.3 dBFS, then MASTER's volume */
    {                                    /* the meter's peak, since the screen last read it */
        float pk = meter_peak;
        for (i = 0; i < n; i++)
            if (fm_fabsf(mono[i]) > pk)
                pk = fm_fabsf(mono[i]);
        meter_peak = pk;
    }
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

/* The tick (1 kHz, the timer interrupt, right after the USB poll): MIDI in, stamped (read here, not in the main loop,
 * so the screen never holds it up). MIDI THRU, as a daisy chain: everything that comes in (notes, CC, clock, Start /
 * Stop; not SysEx, the updater's) goes on at once to the other outs, never back where it came from: USB to Bluetooth,
 * the TRS jack to USB and Bluetooth, Bluetooth to USB (and from a device's link to the computer's, and back).
 * Incoming pulses are held back only while we play on our own clock (ours go out then: no double clock). Then what
 * the audio interrupt sends, when it is heard. */
#define STAMP_OFF ((int32_t)(120 * 24000))       /* 120 ms (the platform's own BLE_TIME_LATE_MS is 100), in ticks */
void app_tick(void)
{
    uint32_t pkt, now = now24();
    unsigned bpm;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, cin = pkt & 0xFu;
        uint32_t from = plat_midi_in_from();
        (void)port;
        if (cin >= 2u && (cin < 4u || cin > 7u) && !(st == 0xF8u && !clock_ext && playing))
            plat_midi_send(pkt & ~0xF0u, MIDI_TO & ~from);   /* (cable 0: the jack's come in as 1) */
        if (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu) {
#ifdef PLAT_HOST
            uint32_t stamp = now;
#else
            uint32_t stamp = plat_midi_in_t24();
#endif
            uint32_t read = now24();
            int32_t age = (int32_t)(read - stamp);
            /* A stamp more than STAMP_OFF from its reading is wrong, not late: Bluetooth's sender times are mapped
             * onto ours by the platform, and one message seemingly sent earlier than any before moves that mapping
             * for all that follow (seen on FMLarynx: 1.5 s). Such a message is taken when it was read: a beat a little
             * less even at worst, never a clock lost */
            if (age > STAMP_OFF || age < -STAMP_OFF)
                stamp = read;
            gk_midi_realtime(st, from, stamp);
        } else if ((st & 0xF0u) == 0x90u && d2 && midiq_w - midiq_r < MIDIQ) {   /* a note: a kick at its pitch */
            midiq[midiq_w % MIDIQ] = (int8_t)fold((int)d1 - ROOT_NOTE);         /* (any channel; not transposed) */
            midiq_w++;
        }
    }
    now = now24();                       /* (after the reads: their stamps may be later than the tick's start) */
    clock_ext = (uint8_t)gk_clock(&bpm, now);
    clock_bpm_x10 = (uint16_t)(bpm > 65535u ? 65535u : bpm);
    while (outq_r != outq_w) {
        uint32_t r = outq_r;
        if ((int32_t)(now - outq[r % OUTQ].at) < 0)
            break;
        plat_midi_send(outq[r % OUTQ].pkt, MIDI_TO);
        outq_r = r + 1u;
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
    uint8_t link, link_row;              /* the connections page is up; its cursor */
    uint32_t kick_at;                    /* the last kick heard (its flash, the playhead, the lights) */
    uint8_t kicked, flash_drawn;         /* one has been; the header last drawn in a flash */
    uint32_t lower_at;                   /* the picture last drawn (the playhead: about 30 times a second) */
    float meter_db;                      /* the meter's level, falling */
    uint32_t meter_at;
    int32_t meter_w;
    uint8_t save_pending;                /* SAVE pressed with no key yet: on its release, the page's action */
    uint32_t link_sig, link_ms;          /* what the page showed, and when (it is drawn again on a change) */
    uint32_t shown_kicks;
    uint8_t redraw;
} ui;

static uint32_t strlen_(const char *s)   /* (no libc on the device) */
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

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
    if (i == KP_SRC) {                   /* SOURCE: the 909, or a key that holds a preset (empty ones skipped) */
        int v = params.v[i], d = detents > 0 ? 1 : -1, k;
        if (!presets.used) {
            app_say("SAVE + KEY", "FIRST");
            return;
        }
        for (k = 0; k < (detents < 0 ? -detents : detents); k++) {
            int w = v + d;
            while (w > 0 && w <= NPRESETS && !(presets.used >> (w - 1) & 1u))
                w += d;
            if (w < 0 || w > NPRESETS)
                break;
            v = w;
        }
        if (v == params.v[i])
            return;
        if (v) {                         /* a new source: the chain blank, so the resample sounds as the preset did */
            params_undo = params;
            has_undo = 1;
            params.v[i] = (int16_t)v;
            kick_params_fresh(&params);
        } else
            params.v[i] = 0;
        resample_at = plat_ms();
    } else
        params.v[i] = (int16_t)kick_param_step(i, params.v[i], detents);
    preset_edited = 1;
    cook();
    kick_param_text(i, params.v[i], t);
    app_say(kick_param_info(i)->name, t);
    ui.focus = slot;
    ui.focus_at = plat_ms();
    ui.wave_dirty = 1;
}

/* ---- the connections page (SEL + REC), in the knobs' place: the Bluetooth MIDI devices found (lib/btdev: the scan
 * while it is up; the one connected), then the USB port's role (DEVICE, to a computer, or HOST to a MIDI device such
 * as another FM-1), as a list: SELECT moves the cursor, SAVE does what the row's action says, SEL + REC goes back.
 * The clock and everything passed on go to all of them; a Start from any of them is followed. As FMLarynx's and
 * Amenity's. */
static const char *upper(const char *s)          /* a device's name as the screen's fonts have it: capitals */
{
    static char b[32];
    uint32_t i;
    for (i = 0; i < sizeof b - 1u && s[i]; i++)
        b[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
    b[i] = 0;
    return b;
}
static int link_devrows(void) { return btdev_busy() || plat_ble_linked() ? 1 : btdev_count() ? btdev_count() : 1; }
static int link_usb_row(void) { return link_devrows(); }
static const char *link_action(void)
{
    if (ui.link_row >= link_usb_row())
        return plat_usb_host_state() == USBH_OFF ? "HOST" : "DEVICE";
    return btdev_busy() ? "LET GO" : plat_ble_linked() ? "" : btdev_count() ? "CONNECT" : "";
}
static void link_enter(void)
{
    if (ui.link_row >= link_usb_row()) {
        plat_usb_host(plat_usb_host_state() == USBH_OFF);
        return;
    }
    if (btdev_busy() || (btdev_count() && !plat_ble_linked())) {
        int ev;
        if (!btdev_busy())
            btdev_choose(ui.link_row);
        ev = btdev_save();
        app_say(ev == BTDEV_CONNECTING ? "CONNECTING" : "BLUETOOTH",
                ev == BTDEV_CONNECTING ? upper(btdev_name()) : btdev_word(ev));
    }
}
static void link_line(int32_t y, int row, const char *label, const char *text, uint16_t c)
{
    int on = ui.link_row == row;
    if (label)
        cv_text(6, y + 2, &FONT_XS, label, C_GRAY);
    if (on)
        cv_text(34, y + 2, &FONT_XS, ">", C_HI);
    cv_text(44, y + 2, &FONT_XS, text, on ? C_HI : c);
}
static void link_draw(void)                      /* the panel under the header: the page */
{
    char t[40], name[24];
    int n = btdev_count(), u = plat_usb_host_state(), rows = link_devrows(), first, i;
    int32_t y = 20;
    cv_begin(240, 90, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    cv_text(6, 4, &FONT_XS, "CONNECTIONS", C_HI);
    if (plat_ble_connected())
        cv_text(234 - text_w(&FONT_XS, "COMPUTER: BT"), 4, &FONT_XS, "COMPUTER: BT", C_GRAY);
    first = ui.link_row < rows ? (int)ui.link_row - 1 : rows - 3;   /* three device rows at most, the cursor's seen */
    first = first > rows - 3 ? rows - 3 : first;
    first = first < 0 ? 0 : first;
    if (btdev_busy()) {
        t[0] = 0;
        text_cat(t, upper(btdev_name()));
        text_cat(t, btdev_ready() ? "  CONNECTED" : "  CONNECTING");
        t[26] = 0;
        link_line(y, 0, "BT", t, btdev_ready() ? C_HI : C_GRAY);
        y += 14;
    } else if (plat_ble_linked()) {              /* the other side connected to us (its name is not known here) */
        link_line(y, 0, "BT", "CONNECTED", C_HI);
        y += 14;
    } else if (!n) {
        link_line(y, 0, "BT", "LOOKING...", C_GRAY);
        y += 14;
    } else
        for (i = first; i < first + 3 && i < n; i++, y += 14)
            if (btdev_found(i, name, 0))
                link_line(y, i, i == first ? "BT" : 0, upper(name), C_GRAY);
    if (u == USBH_OFF)
        link_line(y, rows, "USB", "DEVICE", C_GRAY);
    else if (u == USBH_CONNECTED) {
        const char *dev = upper(plat_usb_host_name());
        t[0] = 0;
        text_cat(t, "HOST TO ");
        text_cat(t, dev[0] ? dev : "A DEVICE");
        t[24] = 0;
        link_line(y, rows, "USB", t, C_HI);
    } else
        link_line(y, rows, "USB", "HOST - LOOKING", C_GRAY);
    if (link_action()[0]) {                      /* what SAVE does on the row: "SAVE=CONNECT" */
        t[0] = 0;
        text_cat(t, "SAVE=");
        text_cat(t, link_action());
        cv_text(6, 78, &FONT_XS, t, C_AMB);
    }
    cv_text(234 - text_w(&FONT_XS, "SEL+REC=BACK"), 78, &FONT_XS, "SEL+REC=BACK", C_DIM);
    cv_blit(0, 30);
}
static void link_poll(void)                      /* every frame: the scan, what happened; the page drawn on a change */
{
    uint32_t sig, now = plat_ms();
    int ev;
    btdev_page(ui.link);
    while ((ev = btdev_event()) != BTDEV_NONE)
        app_say("BLUETOOTH", btdev_word(ev));
    switch (plat_usb_host_msg()) {
    case USBH_MSG_ON: app_say("USB", "HOST"); break;
    case USBH_MSG_OFF: app_say("USB", "DEVICE"); break;
    case USBH_MSG_MIDI:
        app_say("USB HOST TO", plat_usb_host_name()[0] ? upper(plat_usb_host_name()) : "A DEVICE");
        break;
    case USBH_MSG_NOMIDI: app_say("USB", "NOT A MIDI DEVICE"); break;
    case USBH_MSG_LOW: app_say("USB", "NEEDS POWER"); break;
    case USBH_MSG_LOST: app_say("USB", "DEVICE LOST"); break;
    default: break;
    }
    if (ui.link_row > link_usb_row())
        ui.link_row = (uint8_t)link_usb_row();
    if (!ui.link)
        return;
    sig = (uint32_t)btdev_count() | (uint32_t)btdev_pick() << 4 | (uint32_t)btdev_busy() << 8 |
          (uint32_t)btdev_ready() << 9 | (uint32_t)plat_ble_connected() << 10 | (uint32_t)plat_usb_host_state() << 11 |
          (uint32_t)plat_ble_linked() << 13 | (uint32_t)ui.link_row << 14 | 1u << 31;
    if (sig != ui.link_sig || now - ui.link_ms >= 500u) {   /* (the names found may change: twice a second) */
        ui.link_sig = sig;
        ui.link_ms = now;
        ui.redraw = 1;
    }
}

static void page_to(int p)
{
    ui.link = 0;                         /* (a page: the connections page goes) */
    if (p != ui.page)
        ui.wave_dirty = 1;               /* (a page with a spectrum: it is made with the picture) */
    ui.page = p;
    ui.focus = -1;
    app_say("PAGE", PAGES[p].name);
    ui.msg_until = plat_ms() + 3000u;    /* the page's name stays a while */
}

/* ARP: throw the dice (the 909's sweep, click and decay, the EQs, the distortions; the rest stays); SEL + ARP: back
 * to before it */
static void draw(void);

static void busy_draw(const char *word)  /* "RANDOM ...", big, in a box mid-screen, over whatever shows */
{
    int32_t w;
    cv_begin(200, 64, C_BLACK);
    cv_rect(0, 0, 200, 2, C_HI);
    cv_rect(0, 62, 200, 2, C_HI);
    cv_rect(0, 0, 2, 64, C_HI);
    cv_rect(198, 0, 2, 64, C_HI);
    w = text_w(&APP_BIG_FONT, word);
    cv_text(100 - w / 2, 10, &APP_BIG_FONT, word, C_WHITE);
    w = text_w(&FONT_B, "...");
    cv_text(100 - w / 2, 40, &FONT_B, "...", C_HI);
    cv_blit(20, 88);
}

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
        float db;
        params_undo = params;
        has_undo = 1;
        busy_draw("RANDOM");             /* at once, mid-screen: the throw takes a moment (a beat rendered, or two) */
        dice_seed ^= plat_ms() * 2654435761u + ++dice_throws;
        kick_randomize(&params, &dice_seed, &db);
        preset_edited = 1;
        dice_db = (int)(db - 0.5f);
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
                key_kick(53 + (int)k - ROOT_NOTE + xpose);   /* (transposed, as PLAY) */
        }
    if ((btn >> B_PLAY & 1u) && !(ui.btn >> B_PLAY & 1u)) {
        play_req = playing ? RQ_STOP : RQ_START;   /* (the audio interrupt starts or stops the beat) */
        app_say("PLAY", play_req == RQ_START ? "ON" : "STOP");
    }
    {                                    /* SAVE tapped (no key while held) on the connections page: its row's action */
        uint32_t down = btn & ~ui.btn, up = ui.btn & ~btn;
        if (down >> B_SAVE & 1u)
            ui.save_pending = 1;
        if (keys & ~ui.keys)
            ui.save_pending = 0;         /* (SAVE + a key: a preset stored) */
        if ((up >> B_SAVE & 1u) && ui.save_pending) {
            ui.save_pending = 0;
            if (ui.link)
                link_enter();
        }
    }
    if ((btn >> B_ARP & 1u) && !(ui.btn >> B_ARP & 1u))
        dice_throw(row);
    for (k = 0; k < NB; k++)                     /* the page shortcuts */
        if ((btn >> k & 1u) && !(ui.btn >> k & 1u)) {
            int p = -1;
            if (k == B_HOME)                     /* 909; again: SRC, and back */
                p = ui.page == PG_909 ? PG_SRC : PG_909;
            else if (k == B_FX)                  /* D1; again: D2, and back */
                p = ui.page == PG_D1 ? PG_D2 : PG_D1;
            else if (k == B_ENV)                 /* PNCH; again: P.EQ, and back */
                p = ui.page == PG_PUNCH ? PG_PEQ : PG_PUNCH;
            else if (k == B_REC && (btn >> B_SEL & 1u)) {   /* SEL + REC: the connections page, or back */
                ui.link = !ui.link;
                ui.link_sig = 0;
                ui.redraw = 1;
                app_say(ui.link ? "CONNECTIONS" : "PAGE", ui.link ? "" : PAGES[ui.page].name);
            } else if (k == B_REC)               /* PERF; again: ENV, and back */
                p = ui.page == PG_PERF ? PG_ENV : PG_PERF;
            else if (k == B_OCTDN)
                p = PG_SUB;
            else if (k == B_EDIT || k == B_LFO || k == B_GLO || k == B_SEQ || k == B_OCTUP) {   /* an EQ page; */
                int to = k == B_EDIT ? PG_EQ : k == B_LFO ? PG_EQ2 : k == B_GLO ? PG_POST   /* again: next band */
                         : k == B_SEQ ? PG_A : PG_TOT;
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
        if (k == EN_SELECT && ui.link) { /* the connections page: its cursor */
            int r = (int)ui.link_row + (e > 0 ? 1 : -1), last = link_usb_row();
            ui.link_row = (uint8_t)(r < 0 ? 0 : r > last ? last : r);
            if (ui.link_row < link_usb_row() && !btdev_busy() && btdev_count())
                btdev_choose(ui.link_row);
            ui.redraw = 1;
        } else if (k == EN_SELECT) {
            page_to(((ui.page + (e > 0 ? 1 : -1)) % NPAGES + NPAGES) % NPAGES);   /* round: POST, 909, EQ .. */
        } else if (k == EN_PRESET) {
            char t[8];
            tempo = clamp(tempo + e, TEMPO_MIN, TEMPO_MAX);
            int_text(tempo, t);
            app_say("TEMPO", t);
        } else if (k == EN_ALGO) {       /* TRANSPOSE: PLAY's kick and the keys, -12 .. +12 semitones */
            char t[8];
            xpose = (int8_t)clamp(xpose + (e > 0 ? 1 : -1), -12, 12);
            t[0] = xpose > 0 ? '+' : xpose < 0 ? '-' : ' ';
            int_text(xpose < 0 ? -xpose : xpose, t + 1);
            text_cat(t, "ST");
            app_say("TRANSPOSE", t);
        } else if (k >= EN_K1 && k <= EN_K4) {
            param_turn(row * 4 + (int)(k - EN_K1), e);
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    {
        uint32_t now_l = plat_ms(), blink = ui.kicked && now_l - ui.kick_at < 90u;   /* a kick: 90 ms lit */
        uint32_t lit = (playing && blink ? 1u << B_PLAY : 0u) | (btn & (1u << B_SEL | 1u << B_SAVE)) |
                       1u << PAGES[ui.page].button;
        if (blink)                       /* the key PLAY's kick is on (the middle G, transposed) */
            keys |= 1u << (14 + xpose);
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

static void xover_draw(int low, uint16_t color)   /* a half of the crossover */
{
    const kick_coefs_t *c = &coefs[coefs_cur];
    int32_t x, py = db_y(kick_xover_db(c, low, COL_HZ(0)));
    for (x = 0; x < 240; x++) {
        int32_t y = db_y(kick_xover_db(c, low, COL_HZ(x)));
        int32_t y0 = y < py ? y : py, y1 = y < py ? py : y;
        cv_rect(x, 12 + y0, 1, y1 - y0 + 1, color);
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

/* ---- the screen, in three bands (and the meter): the header (it flashes at each kick), the knobs (or the
 * connections page), the picture below; each drawn alone when only it changes, so a flash, the playhead and the meter
 * cost only their own rows */
#define FLASH_MS 150u                    /* the header's flash at a kick, fading out */
#define PLAYHEAD_MS 1000u                /* the playhead runs through the picture's first second */
static uint16_t mix565(uint16_t a, uint16_t b, float f)   /* a to b by f (0..1) */
{
    int32_t ra = a >> 11, ga = (a >> 5) & 63, ba = a & 31, rb = b >> 11, gb = (b >> 5) & 63, bb = b & 31;
    int32_t r = ra + (int32_t)((float)(rb - ra) * f), g = ga + (int32_t)((float)(gb - ga) * f);
    int32_t bl = ba + (int32_t)((float)(bb - ba) * f);
    return (uint16_t)(r << 11 | g << 5 | bl);
}
static uint16_t flash_bg(uint32_t now)   /* the header's ground: the page's colour at a kick, fading to black */
{
    uint32_t dt = now - ui.kick_at;
    if (!ui.kicked || dt >= FLASH_MS)
        return C_BLACK;
    return mix565(C_BLACK, PAGES[ui.page].hi, 0.45f * (float)(FLASH_MS - dt) / (float)FLASH_MS);
}
static void playhead_draw(uint32_t now)  /* the 909 and ENV pictures: a line where the kick is, as it plays */
{
    uint32_t dt = now - ui.kick_at;
    if (ui.kicked && dt < PLAYHEAD_MS)
        cv_rect((int32_t)(dt * 240u / PLAYHEAD_MS), 12, 2, WAVE_H, C_WHITE);
}

static void draw_header(uint32_t now)
{
    char t[24];
    const page_t *pg = &PAGES[ui.page];
    (void)pg;
    /* the header: the name, the tempo and a light for each kick; or a message for a moment */
    cv_begin(240, 30, flash_bg(now));
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
        if (clock_ext) {                 /* an outside clock leads: its tempo, to 0.1 BPM */
            t[0] = 0;
            text_cat(t, "EXT ");
            int_text(clock_bpm_x10 / 10u, t + 4);
            text_cat(t, ".");
            int_text(clock_bpm_x10 % 10u, t + strlen_(t));
        } else
            int_text(tempo, t);
        text_cat(t, " BPM");
        x = 232 - text_w(&FONT_XS, t);
        cv_text(x, 9, &FONT_XS, t, playing ? C_HI : C_GRAY);
        if (xpose) {                     /* TRANSPOSE, left of the tempo */
            t[0] = xpose > 0 ? '+' : '-';
            int_text(xpose < 0 ? -xpose : xpose, t + 1);
            cv_text(x - 8 - text_w(&FONT_XS, t), 9, &FONT_XS, t, C_AMB);
        }
    }
    cv_blit(0, 0);
}

static void draw_panel(uint32_t now)
{
    char t[24];
    uint32_t i;
    int row = (ui.btn >> B_SEL) & 1u;
    /* the page and its parameters: two rows of four, the row the knobs turn lit (or the connections page) */
    if (ui.link)
        link_draw();
    else {
    cv_begin(240, 90, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    {   /* the tabs: the pages of this page's layer, this one lit and underlined, each in its colour; the other
           layers by their name */
        int32_t x = 4, need = 4;
        int k, l = -1, here = PAGE_LAYER[ui.page], shrt;
        for (k = 0; k < NPAGES; k++)     /* the line's width with the other layers' names: too wide, their letters */
            if (PAGE_LAYER[k] == here)
                need += text_w(&FONT_XS, PAGES[k].name) + 4;
            else if (k == 0 || PAGE_LAYER[k] != PAGE_LAYER[k - 1])
                need += text_w(&FONT_XS, LAYER_NAME[PAGE_LAYER[k]]) + 4;
        shrt = need > 236;
        for (k = 0; k < NPAGES; k++) {
            int lay = PAGE_LAYER[k], first = lay != l;
            int32_t x1;
            l = lay;
            if (lay != here) {
                if (first) {
                    x1 = cv_text(x, 4, &FONT_XS, shrt ? LAYER_SHORT[lay] : LAYER_NAME[lay], C_DIM);
                    cv_rect(x - 1, 16, x1 - x + 2, 1, PAGES[k].amb);
                    x = x1 + 4;
                }
                continue;
            }
            x1 = cv_text(x, 4, &FONT_XS, PAGES[k].name, k == ui.page ? PAGES[k].hi : C_DIM);
            cv_rect(x - 1, 16, x1 - x + 2, k == ui.page ? 2 : 1, k == ui.page ? PAGES[k].hi : PAGES[k].amb);
            x = x1 + 4;
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
    }
}

static void draw_lower(uint32_t now)
{
    char t[24];
    uint32_t i;
    int row = (ui.btn >> B_SEL) & 1u;
    const page_t *pg = &PAGES[ui.page];
    (void)row;
    /* the kick (its first second, its spectrum) or the filters' curve; and its pitch */
    cv_begin(240, WAVE_H + 37, C_BLACK);   /* (the meter under it: its own 3 rows) */
    cv_rect(0, 0, 240, 1, C_LINE);
    switch (pg->view) {
    case V_CURVE:                        /* the spectrum after the next distortion, faint; the bands over it */
        freq_grid();
        spec_draw(C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_DIM);   /* 0 dB */
        bands_draw(pg->eq, *band_of_page());
        curve_draw(pg->eq, C_HI);
        break;
    case V_XOVER:                        /* group A's spectrum, faint; the sub's half (with its gain) and the high */
        freq_grid();                     /* half */
        spec_draw(C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_DIM);
        if (params.v[KP_X_FREQ] > kick_param_info(KP_X_FREQ)->min) {
            cv_rect(hz_x((float)params.v[KP_X_FREQ]), 12, 1, WAVE_H, C_AMB);
            xover_draw(0, C_GRAY);
            xover_draw(1, C_HI);
        }
        break;
    case V_SPEC:
        freq_grid();
        spec_draw(C_HI);
        break;
    case V_PERF:                         /* the kick's spectrum, faint; the three peaks' curve, a dot on each */
        freq_grid();
        spec_draw(C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_DIM);
        {
            const kick_coefs_t *c = &coefs[coefs_cur];
            static const uint8_t F[3] = {KP_PF_FREQ, KP_PF_FREQ2, KP_PF_FREQ3};
            int32_t x, py = db_y(kick_perf_db(c, COL_HZ(0)));
            for (x = 0; x < 240; x++) {
                int32_t y = db_y(kick_perf_db(c, COL_HZ(x)));
                int32_t y0 = y < py ? y : py, y1 = y < py ? py : y;
                cv_rect(x, 12 + y0, 1, y1 - y0 + 1, C_HI);
                py = y;
            }
            for (i = 0; i < 3u; i++) {
                int32_t px = hz_x((float)params.v[F[i]]), dy = db_y((float)params.v[KP_PF_AMOUNT]);
                char n[2];
                n[0] = (char)('1' + i);
                n[1] = 0;
                cv_rect(px - 2, 12 + dy - 2, 5, 5, C_AMB);
                cv_text(px + 4, 12 + (dy > 12 ? dy - 11 : dy + 3), &FONT_XS, n, C_GRAY);
            }
        }
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
    case V_ENV:                          /* the kick as heard (ENV on it), faint; ENV's level, +12 .. -24 dB, over it */
        for (i = 1; i < 4u; i++)
            cv_rect((int32_t)i * 60, 12, 1, WAVE_H, C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_LINE);
        for (i = 0; i < 240u; i++)
            cv_rect((int32_t)i, 12 + ui.wave_hi[i], 1, ui.wave_lo[i] - ui.wave_hi[i] + 1, C_DIM);
        playhead_draw(now);
        {
            const kick_coefs_t *c = &coefs[coefs_cur];
            int32_t py = -1, x;
            for (x = 0; x < 240; x++) {  /* a column a 1/240 s */
                int32_t y = (int32_t)((12.0f - kick_env_db(c, (uint32_t)x * (KICK_RATE / 240u))) * (float)(WAVE_H - 1) /
                                      36.0f + 0.5f);
                if (py >= 0 && py != y)
                    cv_rect(x, 12 + (py < y ? py : y), 1, (py < y ? y - py : py - y) + 1, C_HI);
                cv_rect(x, 12 + y, 1, 2, C_HI);
                py = y;
            }
            cv_rect(0, 12 + 12 * (WAVE_H - 1) / 36, 240, 1, C_DIM);   /* 0 dB */
            for (i = 0; i < 3u; i++) {   /* the points */
                int32_t px = params.v[KP_E_TIME + i] * 240 / 1000;
                if (px < 240)
                    cv_rect(px - 2, 12 + (12 - params.v[KP_E_LEVEL + i + 1]) * (WAVE_H - 1) / 36 - 2, 5, 5, C_AMB);
            }
        }
        break;
    default:
        for (i = 1; i < 4u; i++)                 /* 250 ms marks */
            cv_rect((int32_t)i * 60, 12, 1, WAVE_H, C_LINE);
        cv_rect(0, 12 + WAVE_H / 2 - 1, 240, 1, C_LINE);
        for (i = 0; i < 240u; i++)
            cv_rect((int32_t)i, 12 + ui.wave_hi[i], 1, ui.wave_lo[i] - ui.wave_hi[i] + 1, C_HI);
        playhead_draw(now);
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
    note_text(xpose, t);                         /* the root PLAY plays (TRANSPOSE in it) */
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
    cv_blit(0, 120);}

/* the meter: the output's level (after the limiter), a bar along the bottom, -48 .. 0 dBFS; it falls 30 dB a second */
static void draw_meter(int32_t w)
{
    cv_begin(240, 3, C_BLACK);
    if (w > 0)
        cv_rect(0, 0, w, 3, PAGES[ui.page].hi);
    cv_blit(0, 237);
    ui.meter_w = w;
}
static int32_t meter_width(uint32_t now)
{
    float pk = meter_peak, db;
    meter_peak = 0.0f;
    db = pk > 1e-6f ? 6.0206f * fm_log2f(pk) : -120.0f;   /* 20 log10 = 6.02 log2 */
    ui.meter_db -= 0.03f * (float)(now - ui.meter_at);
    ui.meter_at = now;
    if (db > ui.meter_db)
        ui.meter_db = db;
    if (ui.meter_db < -48.0f)
        ui.meter_db = -48.0f;
    return (int32_t)((ui.meter_db + 48.0f) * (240.0f / 48.0f));
}

static void draw(void)
{
    uint32_t now = plat_ms();
    const page_t *pg = &PAGES[ui.page];
    C_AMB = pg->amb;                     /* the page's colours */
    C_HI = pg->hi;
    draw_header(now);
    draw_panel(now);
    draw_lower(now);
    draw_meter(ui.meter_w);
}

/* the splash: the logo (assets/splash.png, 240 x 240, made app/splash.inc by tools/make_splash.py: a palette of 16
 * RGB565 colours and its indices two to a byte) and the version, drawn in two bands of 120 lines */
#include "splash.inc"
void app_splash(void)
{
    uint32_t band, y, x;
    for (band = 0; band < 2u; band++) {
        cv_begin(240, 120, C_BLACK);
        for (y = 0; y < 120u; y++)
            for (x = 0; x < 240u; x++) {
                uint32_t at = (band * 120u + y) * 240u + x;   /* the pixel; its byte, its nibble (left: high) */
                uint8_t b = SPLASH_4BPP[at >> 1];
                cv_pset((int32_t)x, (int32_t)y, SPLASH_PAL[at & 1u ? b & 15u : b >> 4]);
            }
        if (band == 1u) {                /* the version, at the bottom */
            char v[24] = "";
            if (APP_VERSION[0] >= '0' && APP_VERSION[0] <= '9')
                text_cat(v, "V");
            text_cat(v, APP_VERSION);
            cv_text((240 - text_w(&FONT_XS, v)) / 2, 104, &FONT_XS, v, C_GRAY);
        }
        cv_blit(0, band * 120u);
    }
}

#define SPLASH_MS 2500u                  /* the splash, from power-on */
void app_boot(void)
{
#ifndef PLAT_HOST
    while (plat_ms() < SPLASH_MS) {      /* the platform is up: Bluetooth polled, the watchdog fed, meanwhile */
        fm1_wdt_feed();
        plat_yield();
        fm1_delay_ms(10);
    }
#endif
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.page = PG_909;                    /* the 909 first (SRC is before it, for a resample) */
    ui.focus = -1;
    ui.wave_dirty = 1;
    ui.redraw = 1;
}

/* RESAMPLE: the source preset's kick into the buffer, when it is not there yet (another slot, or saved again), once
 * SOURCE has rested a moment; silent meanwhile */
static void resample_check(uint32_t now)
{
    int src = params.v[KP_SRC] - 1;
    kick_params_t sp;
    if (src < 0 || !preset_params(src, &sp)) {
        if (resample_slot >= 0) {
            kick_set_source(0, 0);
            resample_slot = -1;
            cook();
        }
        return;
    }
    if (src == resample_slot && resample_saves == preset_saves)
        return;
    if (now - resample_at < 300u)
        return;
    busy_draw("RESAMPLE");
    muted = 1;
    kick_set_source(0, 0);
    cook();
    kick_set_source(resample_buf, kick_resample(&sp, resample_buf, KICK_SRC_MAX));
    resample_slot = src;
    resample_saves = preset_saves;
    cook();
    muted = 0;
    ui.wave_dirty = 1;
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    input();
    speak();
    resample_check(now);
    link_poll();
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
    if (kicks != ui.shown_kicks) {       /* a kick: the flash, the playhead, the lights from now */
        ui.shown_kicks = kicks;
        ui.kick_at = now;
        ui.kicked = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
        ui.flash_drawn = now - ui.kick_at < FLASH_MS;
        ui.lower_at = now;
    } else {
        const page_t *pg = &PAGES[ui.page];
        int flash = ui.kicked && now - ui.kick_at < FLASH_MS;
        int32_t w = meter_width(now);
        C_AMB = pg->amb;
        C_HI = pg->hi;
        if (flash || ui.flash_drawn) {   /* the header while it flashes, and once more after */
            draw_header(now);
            ui.flash_drawn = (uint8_t)flash;
        }
        if ((pg->view == V_WAVE || pg->view == V_ENV) && !ui.link && ui.kicked &&
            now - ui.kick_at < PLAYHEAD_MS + 60u && now - ui.lower_at >= 33u) {   /* the playhead, 30 a second */
            draw_lower(now);
            ui.lower_at = now;
        } else if (now - ui.kick_at < 60u && now - ui.lower_at >= 33u) {   /* (the last kick's pitch, below) */
            draw_lower(now);
            ui.lower_at = now;
        }
        if (w != ui.meter_w)
            draw_meter(w);
    }
}

#ifdef PLAT_HOST
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)a, (void)b, (void)c, (void)d;
    if (!strcmp(cmd, "splash")) {
        app_splash();
        return 1;
    }
    return 0;
}

int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "playing"))
        *got = playing;
    else if (!strcmp(what, "tempo"))
        *got = tempo;
    else if (!strcmp(what, "link"))      /* the connections page is up */
        *got = ui.link;
    else if (!strcmp(what, "xpose"))
        *got = xpose;
    else if (!strcmp(what, "clocks_out"))   /* clock pulses sent; an outside clock followed */
        *got = (int)clocks_out;
    else if (!strcmp(what, "ext"))
        *got = clock_ext;
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
    else if (!strcmp(what, "resample"))    /* the slot rendered as the source, 1..16; 0 none */
        *got = resample_slot + 1;
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
