#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the offline render (tools/kick_render.c) with the engine the firmware runs: build/kick_render.
# -ffp-contract=off as every other build, so it computes the device's samples.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-cc} -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Iapp/kick -Ibase/include \
    tools/kick_render.c app/kick/kick.c -lm -o build/kick_render
