/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* The render page's engine built natively (web/render/gk_render.c), for tests/render_test.mjs to compare the
 * WebAssembly against: render_ref PRESETS.bin SLOT SEMIS OUT.f32 (the presets object's bytes; the samples, raw
 * float). Prints the length. Build: tools/build_render.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

uint8_t *gk_in(void);
float *gk_out(void);
int gk_load(uint32_t len);
uint32_t gk_render(int slot, int semis);

int main(int argc, char **argv)
{
    FILE *f;
    long len;
    uint32_t n;
    if (argc != 5)
        return 2;
    f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    len = (long)fread(gk_in(), 1, 4096, f);
    fclose(f);
    if (gk_load((uint32_t)len) < 0)
        return 1;
    n = gk_render(atoi(argv[2]), atoi(argv[3]));
    f = fopen(argv[4], "wb");
    fwrite(gk_out(), 4, n, f);
    fclose(f);
    printf("%u\n", n);
    return 0;
}
