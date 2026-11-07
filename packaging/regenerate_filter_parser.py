#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Regenerate the portable filter parser with re2c and SQLite lemon.

The committed output allows native MSVC builds without POSIX host tools.
Use --check to verify the committed files without changing them.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = root / 'engine/Src/FilterEngine'
target = root / 'engine/ports/generated'
with tempfile.TemporaryDirectory(prefix='mergedesk-filter-') as directory:
    temp = Path(directory)
    inputs = {name: (source/name).read_bytes().replace(b'\r\n', b'\n')
              for name in ['FilterLexer.re', 'FilterParser.y']}
    for name, data in inputs.items():
        (temp/name).write_bytes(data)
    subprocess.run(['re2c', '--no-generation-date', '--no-debug-info', 'FilterLexer.re', '-o', 'FilterLexer.cpp'], cwd=temp, check=True)
    command = ['lemon', '-l', '-q']
    for candidate in [Path('/usr/share/lemon/lempar.c'), Path('/usr/local/share/lemon/lempar.c'), Path('/opt/homebrew/opt/lemon/share/lemon/lempar.c')]:
        if candidate.exists():
            command.append('-T'+str(candidate))
            break
    subprocess.run(command+['FilterParser.y'], cwd=temp, check=True)
    for name in ['FilterLexer.cpp', 'FilterParser.c', 'FilterParser.h']:
        data = (temp/name).read_bytes().replace(b'\r\n', b'\n')
        if args.check:
            if (target/name).read_bytes().replace(b'\r\n', b'\n') != data:
                raise SystemExit('Generated parser is stale: '+name)
        else:
            target.mkdir(parents=True, exist_ok=True)
            (target/name).write_bytes(data)
    hashes = '\n'.join(f'set(LM_FILTER_{name.split(".")[0].upper()}_SHA256 "{hashlib.sha256(data).hexdigest()}")' for name, data in inputs.items())+'\n'
    if args.check:
        if (target/'SourceHashes.cmake').read_text() != hashes:
            raise SystemExit('Generated parser source hashes are stale')
    else:
        (target/'SourceHashes.cmake').write_text(hashes)
print('Filter parser verified' if args.check else 'Filter parser regenerated')
