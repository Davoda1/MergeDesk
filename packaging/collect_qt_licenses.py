#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Collect complete notices from the exact official Qt release documentation.

Qt WebEngine does not implement Chrome's chrome://credits page. Its official
release documentation is the authoritative component notice inventory. The
result retains full license text and documentation copyright statements, with
the source URL and SHA-256 of every downloaded page recorded separately.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import html
import json
from pathlib import Path
import re
import urllib.parse
import urllib.request


def download(url: str, cache: Path) -> bytes:
    path = cache / hashlib.sha256(url.encode()).hexdigest()
    if path.is_file():
        return path.read_bytes()
    request = urllib.request.Request(url, headers={'User-Agent': 'MergeDesk-license-packager/1.0'})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read()
    path.write_bytes(data)
    return data


def description(document: str) -> str:
    start = document.index('<div class="descr"')
    end = document.index('<!-- @@@', start)
    return document[start:end].strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    minor = '.'.join(args.version.split('.')[:2])
    base = f'https://doc.qt.io/qt-{minor}/'
    inventory = ['licenses-used-in-qt.html', 'qtwebengine-licensing.html']
    documents = {name: download(base+name, args.cache) for name in inventory}
    if f'Qt {args.version}'.encode() not in documents['qtwebengine-licensing.html']:
        raise RuntimeError('Official documentation does not match the binary Qt patch version')
    names = set(inventory)
    for data in documents.values():
        for name in re.findall(r'href="([^"]+)"', data.decode('utf-8')):
            name = name.split('#')[0]
            if re.fullmatch(r'qt[a-z0-9-]*(?:-attribution-|-3rdparty-)[a-zA-Z0-9-]+\.html', name):
                names.add(name)
    def fetch(name):
        return name, download(base+name, args.cache)
    with ThreadPoolExecutor(max_workers=8) as pool:
        for name, data in pool.map(fetch, sorted(names - documents.keys())):
            documents[name] = data
            print('Collected', name, flush=True)
    records = []
    sections = []
    for name in sorted(documents):
        raw = documents[name]
        page = raw.decode('utf-8')
        title = html.unescape(re.search(r'<title>(.*?)</title>', page, re.S).group(1)).split(' | ')[0]
        body = description(page)
        # Preserve readable links without depending on external CSS or scripts.
        def link(match):
            target = html.unescape(match.group(1))
            if target.startswith(('https:', 'http:', 'mailto:', '#')):
                return match.group(0)
            return 'href="'+html.escape(urllib.parse.urljoin(base+name, target), quote=True)+'"'
        body = re.sub(r'href="([^"]+)"', link, body)
        copyright_match = re.search(r'<p class="copy-notice">.*?</p>', page, re.S)
        if not copyright_match:
            raise RuntimeError(f'Documentation copyright notice missing: {name}')
        copyright_notice = re.sub(r'href="([^"]+)"', link, copyright_match.group(0))
        sections.append(f'<section><h2>{html.escape(title)}</h2><p>Source: <a href="{base+name}">{base+name}</a></p>{body}{copyright_notice}</section>')
        records.append(dict(title=title, url=base+name, sha256=hashlib.sha256(raw).hexdigest()))
    content = '''<!doctype html><html lang="en"><meta charset="utf-8">
<title>Qt third-party notices</title><style>body{max-width:90em;margin:2em auto;font:16px sans-serif;padding:1em}pre{white-space:pre-wrap}section{border-top:1px solid #888;margin-top:3em}.copy-notice{font-size:small}</style>
'''+f'<h1>Qt {html.escape(args.version)} third-party notices</h1><p>Complete component license notices from the official release documentation. The inventory includes components for other Qt modules and platforms; inclusion in this document does not imply that a component is present in MergeDesk. Binary dependency metadata identifies the deployed modules.</p>'+''.join(sections)+'</html>\n'
    (args.output_dir/'component-notices.html').write_text(content, encoding='utf-8')
    (args.output_dir/'component-notices-manifest.json').write_text(json.dumps(dict(qt_version=args.version, pages=records), indent=2)+'\n')
    print(f'Saved {len(records)} complete component notice pages.')


if __name__ == '__main__':
    main()
