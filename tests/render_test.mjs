// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Presethead
// The render page's engine (build/web/gk_render.wasm) against the same code built natively (build/render_ref): every
// used slot of a presets object, at the root and transposed, must give the same kick (the same length, the samples
// within 1e-5). Also: every kick has sound and stays under full scale (the limiter).
//   node tests/render_test.mjs WASM PRESETS.bin REFDIR    REFDIR holds SLOT_SEMIS.f32 from render_ref
import { readFileSync, existsSync } from "node:fs";
const [wasmPath, presetsPath, refDir] = process.argv.slice(2);
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});
const x = instance.exports, presets = readFileSync(presetsPath);
new Uint8Array(x.memory.buffer, x.gk_in(), presets.length).set(presets);
const used = x.gk_load(presets.length);
let fails = 0, n = 0;
if (used < 0) { console.log("FAIL gk_load"); process.exit(1); }
for (let s = 0; s < 16; s++) {
  if (!(used >> s & 1)) { if (x.gk_render(s, 0) !== 0) { console.log(`FAIL slot ${s + 1} empty but rendered`); fails++; } continue; }
  for (const semis of [0, 5, -7]) {
    const len = x.gk_render(s, semis), out = new Float32Array(x.memory.buffer, x.gk_out(), len);
    const ref = `${refDir}/${s}_${semis}.f32`;
    let peak = 0, diff = 0;
    for (let i = 0; i < len; i++) peak = Math.max(peak, Math.abs(out[i]));
    if (existsSync(ref)) {
      const r = new Float32Array(readFileSync(ref).buffer.slice(0));
      if (r.length !== len) { console.log(`FAIL slot ${s + 1} ${semis}: length ${len}, native ${r.length}`); fails++; continue; }
      for (let i = 0; i < len; i++) diff = Math.max(diff, Math.abs(out[i] - r[i]));
    } else { console.log(`FAIL no ${ref}`); fails++; continue; }
    const ok = len > 4410 && peak > 0.1 && peak <= 1.0 && diff < 1e-5;
    console.log(`${ok ? "ok  " : "FAIL"} slot ${s + 1} ${semis >= 0 ? "+" : ""}${semis}: ${(len / 44.1).toFixed(0)} ms, peak ${peak.toFixed(3)}, vs native ${diff.toExponential(1)}`);
    if (!ok) fails++;
    n++;
  }
}
console.log(fails ? `${fails} failed` : `all ${n} renders agree`);
process.exit(fails ? 1 : 0);
