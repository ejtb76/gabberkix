#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Presethead
"""Put the render page together: web/render/page.html with the engine (WASM, inlined as base64) and fm1-base's backup
protocol (web/backup/index.html's, between its "backup protocol" markers) and port finder (web/fm1ports.js), filled in
from app/fm1.toml and app/app.h as fm1-base's make_site.py fills its backup page. One file: it opens as it is, from
disk or from a site.

  tools/make_render_page.py WASM OUT.html        (tools/build_web.sh runs it)"""
import base64
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "base" / "tools"))
import make_site  # noqa: E402  (fm1-base's: backup_values, ports_js, read_conf, fill)


def main(wasm, out):
    app = ROOT / "app"
    conf = make_site.read_conf(app / "fm1.toml")
    values = make_site.backup_values(app, conf)
    backup = (ROOT / "base" / "web" / "backup" / "index.html").read_text(encoding="utf-8")
    m = re.search(r"^// --- backup protocol: begin.*?^// --- backup protocol: end\n", backup, re.S | re.M)
    if not m:
        raise SystemExit("base/web/backup/index.html: no backup protocol markers")
    pb = make_site.fill(m.group(0), dict(values, PORTS_JS=make_site.ports_js()), "the backup protocol")
    page = (ROOT / "web" / "render" / "page.html").read_text(encoding="utf-8")
    page = make_site.fill(page, {"PB_JS": pb, "NAME": conf.get("name", "GABBERKIX"), "EXT": values["EXT"],
                                 "REPO": conf.get("repo", ""),
                                 "WASM_B64": base64.b64encode(Path(wasm).read_bytes()).decode("ascii")}, "web/render")
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text(page, encoding="utf-8", newline="\n")
    print(f"render page: {out} ({len(page) // 1024} KB)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
