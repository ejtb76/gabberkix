// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Presethead
// The render page (build/web/render/index.html) in Node, its script run against a small stand-in for the DOM: a
// backup file opened, every used slot rendered, and the files it makes checked (WAV 24-bit and float, AIFF 24-bit
// and AIFF-C float, the ZIP). Writes them to OUTDIR for a look by other tools.
//   node tests/render_page_test.mjs PAGE.html BACKUP OUTDIR
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import vm from "node:vm";
const [pagePath, bakPath, outDir] = process.argv.slice(2);
mkdirSync(outDir, { recursive: true });
const html = readFileSync(pagePath, "utf8");
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];
const el = () => ({ value: "", hidden: false, disabled: false, textContent: "", className: "", innerHTML: "", files: [],
  addEventListener() {}, append() {}, replaceChildren() {}, querySelector: () => el(), getContext: () => ({ clearRect() {}, fillRect() {}, createLinearGradient: () => ({ addColorStop() {} }) }),
  clientWidth: 200, width: 0, height: 0, click() {}, remove() {}, selected: false });
const els = {};
const document = { getElementById: (id) => (els[id] = els[id] || el()), createElement: el, body: el(), querySelectorAll: () => [] };
const downloads = [];
const ctx = { document, console, atob: (s) => Buffer.from(s, "base64").toString("binary"), TextEncoder, Blob, URL: { createObjectURL: () => "", revokeObjectURL() {} },
  WebAssembly, setTimeout, clearTimeout, devicePixelRatio: 1, addEventListener() {}, navigator: {}, Uint8Array, Float32Array, DataView, ArrayBuffer, Math, Number, String, Array, Date, Promise, Error, Object, Symbol };
vm.createContext(ctx);
vm.runInContext(script + "\n;globalThis.T = { take, fileOf, zip, kicks: () => kicks, PB };", ctx);
let fails = 0;
const check = (ok, what) => { console.log(`${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) fails++; };
for (const id of ["note", "fmt", "bits", "status"]) document.getElementById(id);
els.note.value = "0"; els.fmt.value = "wav"; els.bits.value = "24";
await ctx.T.take(new Uint8Array(readFileSync(bakPath)), "in the test");
const kicks = ctx.T.kicks(), used = kicks.map((k, s) => k ? s : -1).filter((s) => s >= 0);
check(used.length > 0, `${used.length} presets rendered (${els.status.textContent})`);
const s = used[0], k = kicks[s];
for (const [fmt, bits] of [["wav", "24"], ["wav", "32"], ["aiff", "24"], ["aiff", "32"]]) {
  els.fmt.value = fmt; els.bits.value = bits;
  const f = ctx.T.fileOf(s), d = Buffer.from(f.data);
  writeFileSync(`${outDir}/${bits}_${f.name}`, d);
  if (fmt === "wav") {
    const fl = bits === "32", data = d.indexOf("data"), n = d.readUInt32LE(data + 4) / (fl ? 4 : 3);
    const v = fl ? d.readFloatLE(data + 8 + 4 * 1000) : d.readIntLE(data + 8 + 3 * 1000, 3) / 8388607;
    check(d.toString("ascii", 0, 4) === "RIFF" && d.readUInt32LE(4) === d.length - 8 && n === k.length && Math.abs(v - k[1000]) < 2e-7,
          `${f.name} ${bits}-bit: ${n} samples, sample 1000 ${v.toFixed(6)} = ${k[1000].toFixed(6)}`);
  } else {
    const fl = bits === "32", ssnd = d.indexOf("SSND"), comm = d.indexOf("COMM"), n = d.readUInt32BE(comm + 10);
    const v = fl ? d.readFloatBE(ssnd + 16 + 4 * 1000) : d.readIntBE(ssnd + 16 + 3 * 1000, 3) / 8388607;
    check(d.toString("ascii", 8, 12) === (fl ? "AIFC" : "AIFF") && d.readUInt32BE(4) === d.length - 8 && n === k.length &&
          d.readUInt16BE(comm + 16) === 0x400E && Math.abs(v - k[1000]) < 2e-7,
          `${f.name} ${bits}-bit: ${n} frames, sample 1000 ${v.toFixed(6)}`);
  }
}
els.fmt.value = "wav"; els.bits.value = "24";
const blob = ctx.T.zip(used.map((s) => ctx.T.fileOf(s)));
const z = Buffer.from(await blob.arrayBuffer());
writeFileSync(`${outDir}/all.zip`, z);
check(z.readUInt32LE(z.length - 22) === 0x06054B50 && z.readUInt16LE(z.length - 12) === used.length, `ZIP: ${used.length} files, ${z.length} bytes`);
console.log(fails ? `${fails} failed` : "all passed");
process.exit(fails ? 1 : 0);
