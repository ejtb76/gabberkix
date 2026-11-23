/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal; GABBERKIX's changes (C) 2026 Presethead */
/* GABBERKIX's half of the contract with the platform (fm1-base: device/unity.c, host/plat_host.c), from fm1-base's
 * template. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "GABBERKIX"
#define APP_SPLASH_RGB RGB(232, 76, 60)
#define APP_SPLASH                       /* the logo (assets/splash.png) and the version: app_splash, app.c */
#define APP_BOOT                         /* app_boot: the splash stays a while (the start alone takes half a second) */
void app_boot(void);
#define APP_USB_HOST_NAME                /* the USB device hosted, by its own name: HOST TO AMENITY */
#define APP_BTDEV                        /* Bluetooth MIDI devices (lib/btdev.h): the connections page (SEL + REC) */
#define APP_TICK                         /* app_tick: MIDI in (stamped), the clock, THRU, clock out: 1 kHz */
void app_tick(void);
/* backup and restore over USB (fm1.toml backup = true): the presets; the render page (web/render) reads them too */
#define APP_BACKUP_OBJS {OBJ_PRESETS}
#define APP_BACKUP_MAGIC 0x4B42474Bu     /* "KGBK" (the presets object's own magic too) */
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces all presets"
int app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);
#define APP_PALETTE 3u                   /* gfx.c PALETTES: red */
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */

/* the store: the app's objects, the panel calibration (the platform's) and Bluetooth's address and pairing among
 * them, and their flash offsets */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_PRESETS, OBJ_NOBJ };   /* (numbered for good: only ever added) */
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u, 0xDA000u}    /* the presets: the data area, below the panel's */

/* the contract: the platform calls these */
void app_init(void);                     /* boot: the engine and the blank 909 */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */

/* the host simulator's checks (host/plat_host.c "expect"): playing, tempo, kicks (triggered so far), semis (the last
 * kick's), page, row, pN (parameter N's value) */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_CMD                     /* "splash": the boot screen (the simulator's boot skips it) */
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
