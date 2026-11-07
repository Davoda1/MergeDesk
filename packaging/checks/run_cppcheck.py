#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Check application production C++ units with the Qt model and no suppressions."""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('unix64', 'win64'), required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    # main.cpp contains the large embedded GUI selftest harness. Test-only
    # translation units, upstream engines, and Objective-C++ are outside this
    # gate. Native CI separately compiles and tests the application and engines.
    sources = sorted(path for path in (root / 'app').glob('*.cpp')
                     if path.name != 'main.cpp' and not path.match('*Tests*.cpp'))
    if not sources:
        raise RuntimeError('No application C++ sources found')
    subprocess.run(['cppcheck', '--version'], check=True)
    command = ['cppcheck', '--enable=warning,performance,portability',
               '--error-exitcode=1', '--std=c++17', '--library=qt',
               '--template=gcc', '--quiet', f'--platform={args.platform}']
    if args.platform == 'win64':
        command += ['-DQ_OS_WIN', '-D_UNICODE']
    else:
        command += ['-DQ_OS_LINUX', '-DQ_OS_UNIX']
    print(f'Checking {len(sources)} application C++ units for {args.platform}', flush=True)
    return subprocess.run(command + [str(path) for path in sources], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
