#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the render page (web/render): the kick engine (app/kick/kick.c) and web/render/gk_render.c as WebAssembly,
# by zig's clang (pip install ziglang; ZIG="python -m ziglang" by default), then tools/make_render_page.py puts the
# page together: build/web/render/index.html, one file (the engine inlined), opened as it is or put on a site.
# -ffp-contract=off as every other build: the samples the FM-1 makes.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/web
${ZIG:-python -m ziglang} cc --target=wasm32-freestanding -O2 -ffp-contract=off -std=c99 -nostdlib -fno-builtin \
    -Wall -Wextra -Iapp/kick -Ibase/include \
    -Wl,--no-entry -Wl,--export=gk_in -Wl,--export=gk_out -Wl,--export=gk_rate -Wl,--export=gk_load \
    -Wl,--export=gk_render -Wl,--initial-memory=8388608 -Wl,--strip-all -Wl,-z,stack-size=262144 \
    web/render/gk_render.c app/kick/kick.c -o build/web/gk_render.wasm
${PYTHON:-python} tools/make_render_page.py build/web/gk_render.wasm build/web/render/index.html
