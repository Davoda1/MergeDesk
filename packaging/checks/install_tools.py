#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Install the pinned Linux x86-64 repository check tools after verifying SHA-256."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import tarfile
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('The pinned binaries require Linux x86-64')
    manifest = json.loads(Path(__file__).with_name('tools.json').read_text(encoding='utf-8'))
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    limit = 64 * 1024 * 1024
    for name, tool in manifest.items():
        if name not in ('actionlint', 'gitleaks'):
            raise RuntimeError(f'Unknown tool: {name}')
        with urllib.request.urlopen(tool['url'], timeout=60) as response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise RuntimeError(f'Archive exceeds download limit: {name}')
        if hashlib.sha256(data).hexdigest() != tool['sha256']:
            raise RuntimeError(f'Archive checksum mismatch: {name}')
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
            member = archive.getmember(name)
            if not member.isreg() or member.size > limit:
                raise RuntimeError(f'Unexpected binary archive member: {name}')
            stream = archive.extractfile(member)
            if stream is None:
                raise RuntimeError(f'Missing binary archive member: {name}')
            binary = stream.read(limit + 1)
        # Never extract archive paths, links, permissions, or extra files.
        target = destination / name
        target.unlink(missing_ok=True)
        target.write_bytes(binary)
        target.chmod(0o755)
        print(f'Installed {name} {tool["version"]} (archive SHA-256 verified)', flush=True)


if __name__ == '__main__':
    main()
