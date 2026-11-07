#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Run application-level tests using only the packaged private Qt plugin path."""
import argparse,json,os
from pathlib import Path
import subprocess,tempfile,time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--package',type=Path,required=True)
parser.add_argument('--offscreen-web-test',action='store_true')
args=parser.parse_args()
package=args.package.resolve()
executable=package/'mergedesk'
if not executable.is_file() or executable.is_symlink() or executable.read_bytes()[:4]!=b'\x7fELF':
    raise SystemExit('Missing real ELF application at package root')
results=[]
with tempfile.TemporaryDirectory(prefix='mergedesk-package-test-') as directory:
    environment=os.environ.copy()
    for key in ['LD_LIBRARY_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH',
                'QML_IMPORT_PATH','QML2_IMPORT_PATH','QTWEBENGINEPROCESS_PATH',
                'QTWEBENGINE_RESOURCES_PATH','QTWEBENGINE_LOCALES_PATH']:
        environment.pop(key,None)
    environment.update(QT_QPA_PLATFORM='offscreen',XDG_CONFIG_HOME=directory,XDG_CACHE_HOME=directory)
    if os.geteuid()==0: environment['QTWEBENGINE_DISABLE_SANDBOX']='1'
    for name in ['selftest-safety','selftest-mergedesk-features','selftest-theme','selftest-codepage',
                 'selftest-table-edit','selftest-image-options','selftest-tree-sitter','selftest-hex-edit',
                 'selftest-archive','selftest-folder3','selftest-subfolders']:
        start=time.monotonic()
        completed=subprocess.run([executable,'--'+name],cwd=directory,env=environment,text=True,
                                 stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
        results.append(dict(test=name,passed=completed.returncode==0,exit_code=completed.returncode,
                            seconds=round(time.monotonic()-start,3),output=completed.stdout))
        print(('ok: ' if completed.returncode==0 else 'FAIL: ')+name,flush=True)
        if completed.returncode: print(completed.stdout,flush=True)
    # Both virtual display paths exercise Chromium's real software renderer.
    # These overrides belong to headless tests only, never deployed runtime setup.
    environment.update(QTWEBENGINE_CHROMIUM_FLAGS='--disable-gpu',QT_QUICK_BACKEND='software')
    if args.offscreen_web_test:
        environment['QT_QPA_PLATFORM']='offscreen'
        web_command=[executable,'--selftest-web']
    else:
        environment['QT_QPA_PLATFORM']='xcb'
        web_command=['xvfb-run','-a',executable,'--selftest-web']
    start=time.monotonic()
    completed=subprocess.run(web_command,cwd=directory,env=environment,
                             text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
    results.append(dict(test='selftest-web',passed=completed.returncode==0,exit_code=completed.returncode,
                        seconds=round(time.monotonic()-start,3),output=completed.stdout))
    print(('ok: ' if completed.returncode==0 else 'FAIL: ')+'selftest-web',flush=True)
    if completed.returncode: print(completed.stdout,flush=True)
    # Exercise actual Qt open/save/folder requests against a private desktop
    # portal, and prove the mapped plugin comes from this relocated bundle.
    portal_script=Path(__file__).with_name('verify_native_dialogs.py')
    start=time.monotonic()
    completed=subprocess.run(['/usr/bin/python3',portal_script,'--executable',executable,
                              '--expected-plugin-root',package/'plugins',
                              '--output',package/'NATIVE-DIALOGS.json'],
                             cwd=directory,env=environment,text=True,stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT,timeout=180)
    results.append(dict(test='native-file-dialogs',passed=completed.returncode==0,
                        exit_code=completed.returncode,seconds=round(time.monotonic()-start,3),
                        output=completed.stdout))
    print(('ok: ' if completed.returncode==0 else 'FAIL: ')+'native-file-dialogs',flush=True)
    if completed.returncode: print(completed.stdout,flush=True)
(package/'TEST-RESULTS.json').write_text(json.dumps(dict(platform='linux-x86_64',tests=results),indent=2)+'\n')
if any(not test['passed'] for test in results): raise SystemExit('Packaged application tests failed')
