#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Build offline release-preview documentation from actual Qt app screenshots.

Usage:
  python3 packaging/create_preview.py qa/previews qa/MergeDesk-preview.html

PNG filenames may end in -light, -dark or -system, for example text-light.png,
folder-dark.png or image-light.png. The preceding name becomes the comparison
mode. Every PNG is embedded; the HTML needs no server, network, font or asset
files. Controls select screenshots only. This does not implement a web version
of MergeDesk. Only supply captures of the actual application, not illustrations.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import html
import json
from pathlib import Path
import re
import struct

MODES = {
    "text": ("Text", "Follow every change", "Aligned text panes, syntax colors, word differences and a compact difference map."),
    "three-way": ("Three-way text", "Keep three versions in view", "Compare related text versions in the native desktop editor."),
    "threeway": ("Three-way text", "Keep three versions in view", "Compare related text versions in the native desktop editor."),
    "folder": ("Folders", "See the shape of a change", "Explore recursive folder comparisons and their file-level results."),
    "folders": ("Folders", "See the shape of a change", "Explore recursive folder comparisons and their file-level results."),
    "table": ("Tables", "Compare by row and column", "Inspect delimited data in the editable native table view."),
    "csv": ("Tables", "Compare by row and column", "Inspect delimited data in the editable native table view."),
    "image": ("Images", "Look closer at the pixels", "Compare images with the native image view and WinIMerge engine."),
    "images": ("Images", "Look closer at the pixels", "Compare images with the native image view and WinIMerge engine."),
    "binary": ("Binary", "Inspect the underlying bytes", "Compare binary data in the native hex editor."),
    "hex": ("Binary", "Inspect the underlying bytes", "Compare binary data in the native hex editor."),
    "web": ("Webpages", "Compare rendered pages", "View actual page rendering in Qt WebEngine and compare captured snapshots."),
    "webpage": ("Webpages", "Compare rendered pages", "View actual page rendering in Qt WebEngine and compare captured snapshots."),
    "document": ("Documents", "Bring document text into view", "Read supported document text through native extraction or configured converters."),
}
THEMES = {"light": "Light", "dark": "Dark", "system": "System", "capture": "Captured appearance"}


STYLE = r"""
:root{color-scheme:light;--paper:#f4f5f7;--card:#fff;--ink:#192736;--muted:#596776;--line:#dce2e8;--accent:#176a65;--accent-soft:#e4f2ef;--frame:#e9edf1;--shadow:0 18px 58px #152b4014;--focus:#156bbd}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;font-family:ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:var(--paper);color:var(--ink);line-height:1.5}
body[data-appearance=dark]{color-scheme:dark;--paper:#111923;--card:#19242f;--ink:#eef2f5;--muted:#b1bdc9;--line:#35424f;--accent:#88d7c8;--accent-soft:#213e3a;--frame:#0d151e;--shadow:0 20px 65px #0004;--focus:#8ac8ff}
button{font:inherit}button:focus-visible,a:focus-visible,summary:focus-visible{outline:3px solid var(--focus);outline-offset:4px}button{cursor:pointer}button:disabled{cursor:default}a{color:var(--accent)}
.shell{width:min(1196px,calc(100% - 64px));margin:auto}.masthead{display:flex;align-items:center;justify-content:space-between;min-height:92px;border-bottom:1px solid var(--line);gap:20px}.brand{display:flex;align-items:center;gap:12px;font-size:21px;font-weight:750;letter-spacing:-.04em}.brand-mark{display:grid;grid-template-columns:repeat(2,9px);gap:4px;padding:10px 12px;border:1px solid var(--line);border-radius:11px;background:var(--card);height:44px;width:46px}.brand-mark span{border-radius:2px;background:var(--accent);height:22px}.brand-mark span+span{height:14px;margin-top:4px;opacity:.5}.top-label{font-size:12px;color:var(--muted);letter-spacing:.08em;text-transform:uppercase}.hero{padding:63px 0 44px;display:grid;grid-template-columns:1fr 285px;align-items:end;gap:52px}.eyebrow{font-size:12px;font-weight:750;letter-spacing:.13em;text-transform:uppercase;color:var(--accent);margin:0 0 17px}.hero h1{font-size:clamp(38px,4.2vw,56px);line-height:1.09;letter-spacing:-.05em;margin:0 0 23px;font-weight:760}.hero p.lead{font-size:17px;line-height:1.65;color:var(--muted);max-width:730px;margin:0}.release-facts{padding:23px;border:1px solid var(--line);border-radius:16px;background:var(--card)}.release-facts p{margin:0 0 16px;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted)}.release-facts dl{margin:0;display:grid;grid-template-columns:72px 1fr;gap:10px 15px;font-size:13px}.release-facts dt{color:var(--muted)}.release-facts dd{margin:0;font-weight:600}.release-facts dd:last-child{font-variant-numeric:tabular-nums}.gallery-head{display:flex;align-items:flex-end;justify-content:space-between;gap:24px;margin:0 0 17px}.gallery-head h2{font-size:22px;letter-spacing:-.025em;margin:0 0 4px}.gallery-head p{font-size:13px;color:var(--muted);margin:0}.capture-badge{display:flex;align-items:center;gap:7px;color:var(--accent);font-size:12px;font-weight:650;white-space:nowrap}.capture-badge:before{content:"";width:7px;height:7px;background:var(--accent);border-radius:100%}.gallery{border:1px solid var(--line);border-radius:18px;background:var(--card);box-shadow:var(--shadow);overflow:hidden}.controls{display:none;align-items:center;justify-content:space-between;gap:18px;flex-wrap:wrap;padding:17px 20px;border-bottom:1px solid var(--line)}.js .controls{display:flex}.mode-controls,.theme-controls{display:flex;align-items:center;gap:5px;flex-wrap:wrap}.mode-controls button{background:transparent;border:0;color:var(--muted);border-radius:7px;padding:7px 11px;font-size:13px;font-weight:650}.mode-controls button[aria-pressed=true]{color:var(--accent);background:var(--accent-soft)}.theme-controls{border:1px solid var(--line);border-radius:9px;padding:3px;gap:2px}.theme-controls button{background:transparent;border:0;color:var(--muted);border-radius:6px;padding:5px 10px;font-size:12px}.theme-controls button[aria-pressed=true]{color:var(--ink);background:var(--frame);font-weight:650}.theme-controls button:disabled{opacity:.35}.screen{margin:0}.screen+.screen{border-top:1px solid var(--line)}.screen.is-hidden{display:none}.screen-frame{background:var(--frame);padding:21px 20px;display:flex;justify-content:center;align-items:center}.screen img{height:auto;display:block;max-width:100%;border:1px solid var(--line);box-shadow:0 3px 15px #00000018;border-radius:3px}.screen figcaption{padding:21px 24px;display:flex;justify-content:space-between;align-items:flex-start;gap:30px}.caption-title{font-size:16px;font-weight:700;letter-spacing:-.015em;margin-bottom:4px}.caption-detail{font-size:13px;color:var(--muted);max-width:720px}.capture-meta{font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:11px;color:var(--muted);text-align:right;line-height:1.8;white-space:nowrap}.gallery-footer{display:none;align-items:center;justify-content:space-between;gap:18px;padding:13px 24px;border-top:1px solid var(--line);background:var(--card);font-size:12px;color:var(--muted)}.js .gallery-footer{display:flex}.zoom-button{border:0;padding:4px 0;color:var(--accent);font-size:12px;font-weight:650;background:transparent}.context{display:grid;grid-template-columns:repeat(3,1fr);gap:30px;margin:40px 0}.context article{border-top:2px solid var(--line);padding-top:19px}.context h3{font-size:14px;margin:0 0 8px}.context p{font-size:13px;color:var(--muted);margin:0;line-height:1.7}.provenance{font-size:12px;color:var(--muted);border-top:1px solid var(--line);padding-top:19px;margin-bottom:27px}.provenance summary{width:fit-content;cursor:pointer;color:var(--ink);font-weight:600}.provenance p{max-width:920px;line-height:1.75}.provenance table{width:100%;border-collapse:collapse;margin:16px 0;font-size:11px;text-align:left}.provenance th,.provenance td{border-bottom:1px solid var(--line);padding:8px 10px;vertical-align:top;overflow-wrap:anywhere}.provenance th:first-child,.provenance td:first-child{padding-left:0}.provenance code{font-family:ui-monospace,Consolas,monospace;font-size:10px}.foot{display:flex;justify-content:space-between;gap:16px;padding:0 0 31px;color:var(--muted);font-size:11px}dialog{border:1px solid var(--line);border-radius:12px;padding:0;color:var(--ink);background:var(--card);max-width:calc(100vw - 32px);max-height:calc(100vh - 32px)}dialog::backdrop{background:#07111cce;backdrop-filter:blur(4px)}.dialog-head{display:flex;justify-content:space-between;align-items:center;padding:12px 18px;gap:30px;font-size:13px;border-bottom:1px solid var(--line)}.dialog-close{border:1px solid var(--line);border-radius:5px;background:transparent;color:var(--ink);padding:3px 9px}.dialog-body{overflow:auto;max-height:calc(100vh - 104px);padding:12px;background:var(--frame)}.dialog-body img{display:block;max-width:none;height:auto}.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
@media(max-width:880px){.hero{grid-template-columns:1fr;gap:25px;padding-top:44px}.release-facts{max-width:none;padding:17px 20px}.release-facts p{display:none}.release-facts dl{grid-template-columns:auto auto auto auto;gap:9px 15px}.release-facts dt:last-of-type,.release-facts dd:last-of-type{display:none}.gallery-head{align-items:flex-start}.context{gap:20px}.screen-frame{padding:12px}.capture-meta{white-space:normal}}
@media(max-width:600px){.shell{width:calc(100% - 32px)}.masthead{min-height:75px}.top-label{font-size:10px}.hero{padding-top:32px}.hero h1{font-size:38px}.hero p.lead{font-size:15px}.release-facts dl{font-size:12px}.gallery-head{display:block}.capture-badge{margin-top:9px}.controls{padding:12px}.screen-frame{padding:6px}.screen figcaption{padding:16px;display:block}.capture-meta{margin-top:9px;text-align:left}.context{grid-template-columns:1fr;gap:23px;margin:29px 0}.gallery-footer{padding:12px 16px;align-items:flex-start;flex-direction:column;gap:7px}.foot{display:block}.foot span{display:block;margin-top:5px}.provenance th:last-child,.provenance td:last-child{display:none}}
@media(prefers-reduced-motion:reduce){html{scroll-behavior:auto}}
@media print{body,body[data-appearance=dark]{--paper:#fff;--card:#fff;--ink:#111;--muted:#555;--line:#bbb;--frame:#eee;--accent:#235b52;background:#fff;color:#111}.shell{width:100%}.hero{padding:25px 0}.hero h1{font-size:34px}.gallery{box-shadow:none}.controls,.gallery-footer,.capture-badge,dialog{display:none!important}.screen.is-hidden{display:block}.screen{break-inside:avoid;page-break-inside:avoid}.screen-frame{padding:8px}.context{margin:24px 0}.provenance table{font-size:9px}.foot{font-size:9px}}
"""

SCRIPT = r"""
(() => {
  const figures = [...document.querySelectorAll('.screen')];
  const modeButtons = [...document.querySelectorAll('[data-select-mode]')];
  const themeButtons = [...document.querySelectorAll('[data-select-theme]')];
  let currentMode = figures[0].dataset.mode;
  let currentTheme = figures[0].dataset.theme;
  const available = mode => figures.filter(item => item.dataset.mode === mode);
  function select(mode, theme) {
    const candidates = available(mode);
    const chosen = candidates.find(item => item.dataset.theme === theme) || candidates[0];
    currentMode = mode; currentTheme = chosen.dataset.theme;
    figures.forEach(item => item.classList.toggle('is-hidden', item !== chosen));
    modeButtons.forEach(button => button.setAttribute('aria-pressed', String(button.dataset.selectMode === mode)));
    themeButtons.forEach(button => {
      button.disabled = !candidates.some(item => item.dataset.theme === button.dataset.selectTheme);
      button.setAttribute('aria-pressed', String(button.dataset.selectTheme === currentTheme));
    });
    document.body.dataset.appearance = currentTheme === 'dark' ? 'dark' : 'light';
    document.querySelector('#selection-label').textContent = chosen.dataset.label;
    document.querySelector('#capture-status').textContent = chosen.dataset.label + ' selected.';
  }
  modeButtons.forEach(button => button.addEventListener('click', () => select(button.dataset.selectMode, currentTheme)));
  themeButtons.forEach(button => button.addEventListener('click', () => select(currentMode, button.dataset.selectTheme)));
  const dialog = document.querySelector('#capture-dialog');
  document.querySelector('#open-capture').addEventListener('click', () => {
    const chosen = figures.find(item => !item.classList.contains('is-hidden'));
    const original = chosen.querySelector('img');
    const enlarged = dialog.querySelector('img');
    enlarged.src = original.src; enlarged.alt = original.alt;
    enlarged.width = original.width; enlarged.height = original.height;
    document.querySelector('#dialog-title').textContent = chosen.dataset.label + ' · original pixels';
    if (dialog.showModal) dialog.showModal();
    else chosen.scrollIntoView({block:'center'});
  });
  document.querySelector('#close-capture').addEventListener('click', () => dialog.close());
  dialog.addEventListener('click', event => { if (event.target === dialog) dialog.close(); });
  select(currentMode, currentTheme);
})();
"""


def read_screenshots(directory: Path) -> list[dict[str, object]]:
    files = sorted((path for path in directory.iterdir() if path.is_file() and path.suffix.lower() == ".png"),
                   key=lambda path: path.name.casefold())
    if not files:
        raise ValueError(f"No PNG screenshots found in {directory}")
    screens = []
    combinations = set()
    for path in files:
        data = path.read_bytes()
        if len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
            raise ValueError(f"Not a PNG with a valid header: {path.name}")
        width, height = struct.unpack(">II", data[16:24])
        if not width or not height:
            raise ValueError(f"Invalid screenshot dimensions: {path.name}")
        match = re.fullmatch(r"(.+?)[-_](light|dark|system)", path.stem, re.IGNORECASE)
        mode = match.group(1).lower().replace("_", "-") if match else path.stem.lower().replace("_", "-")
        theme = match.group(2).lower() if match else "capture"
        if (mode, theme) in combinations:
            raise ValueError(f"Duplicate mode/appearance: {mode}/{theme}. Rename {path.name}.")
        combinations.add((mode, theme))
        title, heading, detail = MODES.get(mode, (mode.replace("-", " ").title(),
                                                  "Inside the native application", "Actual Qt desktop application capture."))
        screens.append({"filename": path.name, "mode": mode, "theme": theme,
                        "title": title, "heading": heading, "detail": detail,
                        "width": width, "height": height, "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "image": "data:image/png;base64," + base64.b64encode(data).decode("ascii")})
    priority = {mode: number for number, mode in enumerate(MODES)}
    theme_priority = {"light": 0, "dark": 1, "system": 2, "capture": 3}
    return sorted(screens, key=lambda screen: (priority.get(str(screen["mode"]), 100),
                                              screen["mode"], theme_priority[str(screen["theme"])]))


def build_html(screens: list[dict[str, object]], version: str, platform_name: str) -> str:
    escape = lambda value: html.escape(str(value), quote=True)
    mode_buttons = []
    seen_modes = set()
    for screen in screens:
        mode = str(screen["mode"])
        if mode not in seen_modes:
            mode_buttons.append(f'<button type="button" data-select-mode="{escape(mode)}" aria-pressed="false">'
                                f'{escape(screen["title"])}</button>')
            seen_modes.add(mode)
    themes = [key for key in THEMES if any(screen["theme"] == key for screen in screens)]
    theme_buttons = [f'<button type="button" data-select-theme="{theme}" aria-pressed="false">{label}</button>'
                     for theme, label in THEMES.items() if theme in themes]
    figures = []
    manifest_rows = []
    for index, screen in enumerate(screens):
        theme_label = THEMES[str(screen["theme"])]
        label = f'{screen["title"]} / {theme_label}'
        dimensions = f'{screen["width"]} × {screen["height"]}'
        figures.append(f'''<figure class="screen" data-mode="{escape(screen['mode'])}" data-theme="{escape(screen['theme'])}" data-label="{escape(label)}">
<div class="screen-frame"><img src="{screen['image']}" width="{screen['width']}" height="{screen['height']}" alt="Actual MergeDesk Qt application: {escape(label)}. {escape(screen['detail'])}" loading="{'eager' if index == 0 else 'lazy'}" decoding="async"></div>
<figcaption><div><div class="caption-title">{escape(screen['heading'])}</div><div class="caption-detail">{escape(screen['detail'])}</div></div><div class="capture-meta">{escape(label)}<br>{dimensions} px · actual app capture</div></figcaption></figure>''')
        manifest_rows.append(f'<tr><td>{escape(screen["filename"])}</td><td>{dimensions} px</td>'
                             f'<td><code>{screen["sha256"]}</code></td></tr>')
    manifest = {"generator": "MergeDesk packaging/create_preview.py", "schemaVersion": 1,
                "generatedUtc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "applicationVersion": version, "capturePlatform": platform_name,
                "screenshots": [{key: value for key, value in screen.items() if key != "image"} for screen in screens]}
    embedded_manifest = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
    count = len(screens)
    mode_count = len(seen_modes)
    return f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="description" content="Actual screenshots of the MergeDesk {escape(version)} native Qt 6 desktop application.">
<meta name="color-scheme" content="light dark"><title>MergeDesk {escape(version)} — Desktop preview</title>
<style>{STYLE}</style><script>document.documentElement.classList.add('js');</script></head>
<body data-appearance="light"><div class="shell">
<header class="masthead"><div class="brand"><span class="brand-mark" aria-hidden="true"><span></span><span></span></span>MergeDesk</div><div class="top-label">Desktop release preview</div></header>
<main><section class="hero" aria-labelledby="headline"><div><p class="eyebrow">Version {escape(version)} · Native Qt 6</p>
<h1>A familiar diff engine.<br>A native desktop.</h1>
<p class="lead">Text, folders, tables, images and binary data, brought together in a Qt 6 desktop application with light and dark appearance.</p></div>
<aside class="release-facts" aria-label="Release details"><p>At a glance</p><dl><dt>Targets</dt><dd>Windows · GNU/Linux</dd><dt>Interface</dt><dd>Qt 6 Widgets</dd><dt>License</dt><dd>GPL-3.0-or-later</dd></dl></aside></section>
<section aria-labelledby="gallery-heading"><div class="gallery-head"><div><h2 id="gallery-heading">The application, in view</h2><p>Browse actual captures from the running {escape(platform_name)} application.</p></div><span class="capture-badge">Actual Qt screenshots</span></div>
<div class="gallery"><div class="controls"><div class="mode-controls" role="group" aria-label="Comparison mode">{''.join(mode_buttons)}</div><div class="theme-controls" role="group" aria-label="Screenshot appearance">{''.join(theme_buttons)}</div></div>
{''.join(figures)}
<div class="gallery-footer"><span id="selection-label"></span><button type="button" class="zoom-button" id="open-capture">Open full-size capture ↗</button></div></div>
<p id="capture-status" class="sr-only" role="status" aria-live="polite"></p></section>
<section class="context" aria-label="About this preview"><article><h3>Native controls, familiar workflow</h3><p>The comparison views run in the desktop Qt application. The gallery controls above switch between captured views.</p></article><article><h3>Light, dark, or follow the system</h3><p>MergeDesk offers system, light and dark appearance. Available screenshots show the actual theme colors and comparison highlights.</p></article><article><h3>An established engine lineage</h3><p>The application builds on LibreMerge’s Qt work and the WinMerge comparison engines, with original attributions retained.</p></article></section>
<details class="provenance"><summary>Capture details and source attribution</summary><p>This offline preview embeds {count} PNG screenshots across {mode_count} comparison {'mode' if mode_count == 1 else 'modes'}. Captures are from the actual {escape(platform_name)} Qt application, not illustrations. The HTML is release documentation; editing and comparison happen in the desktop application. These captures establish appearance on the stated platform and do not establish runtime validation on other platforms.</p>
<p>MergeDesk additions: Copyright © 2026 davoda1, GPL-3.0-or-later. The source distribution contains the upstream WinMerge and LibreMerge notices, complete license texts, and <code>docs/PROVENANCE.md</code>. The compatibility matrix in <code>docs/COMPATIBILITY.md</code> describes supported features and remaining boundaries.</p>
<table><thead><tr><th scope="col">Source capture</th><th scope="col">Dimensions</th><th scope="col">SHA-256</th></tr></thead><tbody>{''.join(manifest_rows)}</tbody></table></details>
</main><footer class="foot"><span>MergeDesk {escape(version)} · Copyright © 2026 davoda1</span><span>Self-contained release preview · {count} actual application captures</span></footer></div>
<dialog id="capture-dialog" aria-labelledby="dialog-title"><div class="dialog-head"><span id="dialog-title">Original capture</span><button type="button" class="dialog-close" id="close-capture" autofocus>Close</button></div><div class="dialog-body"><img alt="Full-size application capture"></div></dialog>
<script type="application/json" id="capture-manifest">{embedded_manifest}</script><script>{SCRIPT}</script></body></html>'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("screenshots", type=Path, help="Directory of actual application PNG screenshots")
    parser.add_argument("output", type=Path, help="Self-contained output HTML path")
    parser.add_argument("--version", default="1.0.0", help="Application version displayed in the document")
    parser.add_argument("--capture-platform", default="GNU/Linux", help="Platform on which the screenshots were captured")
    args = parser.parse_args()
    directory = args.screenshots.expanduser().resolve()
    if not directory.is_dir():
        parser.error(f"Screenshot directory does not exist: {directory}")
    try:
        screens = read_screenshots(directory)
        document = build_html(screens, args.version, args.capture_platform)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    output = args.output.expanduser().resolve()
    if output.suffix.lower() not in (".html", ".htm"):
        parser.error("Output must use an .html or .htm extension")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(document, encoding="utf-8")
    print(f"Created {output} ({output.stat().st_size:,} bytes; {len(screens)} embedded actual Qt screenshots)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
