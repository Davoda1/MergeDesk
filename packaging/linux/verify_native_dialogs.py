#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
# Builds on LibreMerge's GPL-3.0-or-later packaging/fake_portal.py fixture.
"""Exercise MergeDesk's real Qt native choosers on a private D-Bus session.

Requires python3-dbus, python3-gi, dbus-run-session and an X display (or
xvfb-run). No desktop portal or settings on the user's session bus are changed.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--expected-plugin-root', type=Path)
    parser.add_argument('--inside-private-bus', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    output = args.output.resolve() if args.output else None
    plugin_root = args.expected_plugin_root.resolve() if args.expected_plugin_root else None

    if not args.inside_private_bus:
        command = ['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()),
                   '--inside-private-bus', '--executable', str(executable)]
        if output:
            command += ['--output', str(output)]
        if plugin_root:
            command += ['--expected-plugin-root', str(plugin_root)]
        if not os.environ.get('DISPLAY'):
            command = ['xvfb-run', '-a'] + command
        environment = os.environ.copy()
        environment['MERGEDESK_PRIVATE_DIALOG_BUS'] = '1'
        return subprocess.run(command, env=environment, timeout=150).returncode

    if os.environ.get('MERGEDESK_PRIVATE_DIALOG_BUS') != '1':
        raise RuntimeError('The fixture must start its own private D-Bus session')

    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib

    DBusGMainLoop(set_as_default=True)
    session = dbus.SessionBus()
    loop = GLib.MainLoop()
    requests = []
    runs = []
    overrides = []
    failures = []
    portal_path = '/org/freedesktop/portal/desktop'
    chooser_interface = 'org.freedesktop.portal.FileChooser'
    settings_interface = 'org.freedesktop.portal.Settings'

    def plain(value):
        if isinstance(value, dbus.Boolean):
            return bool(value)
        if isinstance(value, dict):
            return {str(key): plain(item) for key, item in value.items()}
        if isinstance(value, (list, tuple, bytes)):
            return [plain(item) for item in value]
        if isinstance(value, str):
            return str(value)
        if isinstance(value, int):
            return int(value)
        return value

    class Request(dbus.service.Object):
        @dbus.service.signal('org.freedesktop.portal.Request', signature='ua{sv}')
        def Response(self, status, results):
            pass

        @dbus.service.method('org.freedesktop.portal.Request', in_signature='', out_signature='')
        def Close(self):
            pass

        def complete(self, uri):
            self.Response(dbus.UInt32(0), {'uris': dbus.Array([uri], signature='s')})
            return False

    class Portal(dbus.service.Object):
        def __init__(self, bus, directory):
            super().__init__(bus, portal_path)
            self.directory = directory
            self.desktop = ''
            self.pending = []
            self.child_pid = None

        @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
        def Get(self, interface, name):
            if (interface, name) == (chooser_interface, 'version'):
                return dbus.UInt32(4, variant_level=1)
            raise dbus.exceptions.DBusException('Unknown property')

        @dbus.service.method(settings_interface, in_signature='ss', out_signature='v')
        def Read(self, namespace, key):
            if (namespace, key) == ('org.freedesktop.appearance', 'color-scheme'):
                return dbus.UInt32(0, variant_level=1)
            raise dbus.exceptions.DBusException('Unknown setting')

        @dbus.service.method(settings_interface, in_signature='ss', out_signature='v')
        def ReadOne(self, namespace, key):
            return self.Read(namespace, key)

        def choose(self, method, parent, title, options, sender):
            targets = {'MergeDesk native open': 'input.txt',
                       'MergeDesk native save': 'output.txt',
                       'MergeDesk native folder': 'selected-directory'}
            if title not in targets:
                raise dbus.exceptions.DBusException('Unexpected chooser request')
            uri = (self.directory / targets[str(title)]).as_uri()
            token = str(options.get('handle_token', 'mergedesk'))
            handle = portal_path + '/request/' + str(sender)[1:].replace('.', '_') + '/' + token
            pid_source = 'D-Bus credentials'
            try:
                pid = int(session.call_blocking('org.freedesktop.DBus', '/org/freedesktop/DBus',
                          'org.freedesktop.DBus', 'GetConnectionUnixProcessID', 's', (sender,)))
                pids = [pid] if pid > 0 else []
            except dbus.exceptions.DBusException:
                pids = []
            if not pids:
                # A private TCP bus has no Unix socket credentials. Inspect
                # only this fixture's launched child and its descendants.
                pid_source = 'launched process tree (bus has no Unix PID)'
                pids = [self.child_pid] if self.child_pid else []
                for pid in pids:
                    try:
                        children = Path(f'/proc/{pid}/task/{pid}/children').read_text().split()
                        pids.extend(int(child) for child in children if int(child) not in pids)
                    except OSError:
                        pass
            loaded = set()
            maps_readable = False
            map_errors = []
            for pid in pids:
                try:
                    maps = Path(f'/proc/{pid}/maps').read_text()
                    maps_readable = True
                    loaded.update(line.rsplit(None, 1)[-1].replace('\\040', ' ')
                                  for line in maps.splitlines()
                                  if line.endswith('/libqxdgdesktopportal.so'))
                except OSError as error:
                    map_errors.append(dict(pid=pid, error=type(error).__name__))
            requests.append(dict(desktop=self.desktop, method=method, parent=str(parent),
                                 title=str(title), options=plain(options), response_code=0,
                                 response_uris=[uri], portal_plugins=sorted(loaded),
                                 client_processes=pids, pid_source=pid_source,
                                 maps_readable=maps_readable, map_errors=map_errors,
                                 plugin_evidence='process maps' if loaded else 'unavailable'))
            request = Request(session, handle)
            self.pending.append(request)
            # Reply first, so Qt can subscribe to this request's Response signal.
            GLib.timeout_add(250, request.complete, uri)
            return dbus.ObjectPath(handle)

        @dbus.service.method(chooser_interface, in_signature='ssa{sv}', out_signature='o', sender_keyword='sender')
        def OpenFile(self, parent, title, options, sender):
            return self.choose('OpenFile', parent, title, options, sender)

        @dbus.service.method(chooser_interface, in_signature='ssa{sv}', out_signature='o', sender_keyword='sender')
        def SaveFile(self, parent, title, options, sender):
            return self.choose('SaveFile', parent, title, options, sender)

    def run(arguments, environment):
        with tempfile.TemporaryFile(mode='w+', encoding='utf-8') as capture:
            child = subprocess.Popen([str(executable)] + arguments, env=environment,
                                     stdout=capture, stderr=subprocess.STDOUT,
                                     cwd=environment['XDG_CONFIG_HOME'])
            portal.child_pid = child.pid
            started = time.monotonic()

            def reap():
                if child.poll() is None and time.monotonic() - started < 30:
                    return True
                if child.poll() is None:
                    child.kill()
                    child.wait()
                loop.quit()
                return False

            GLib.timeout_add(25, reap)
            loop.run()
            capture.seek(0)
            return child.returncode, capture.read()

    with tempfile.TemporaryDirectory(prefix='mergedesk-native-dialogs-') as temporary:
        directory = Path(temporary)
        (directory / 'input.txt').write_text('Native chooser fixture\n', encoding='utf-8')
        (directory / 'selected-directory').mkdir()
        for name in ('config', 'cache', 'data', 'runtime'):
            (directory / name).mkdir(mode=0o700)
        portal = Portal(session, directory)
        service = dbus.service.BusName('org.freedesktop.portal.Desktop', session, do_not_queue=True)
        environment = os.environ.copy()
        for key in ('QT_QPA_PLATFORMTHEME', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
                    'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'LD_LIBRARY_PATH', 'LD_PRELOAD',
                    'SNAP', 'FLATPAK_ID', 'KDE_FULL_SESSION'):
            environment.pop(key, None)
        environment.update(QT_QPA_PLATFORM='xcb', XDG_CONFIG_HOME=str(directory / 'config'),
                           XDG_CACHE_HOME=str(directory / 'cache'), XDG_DATA_HOME=str(directory / 'data'),
                           XDG_RUNTIME_DIR=str(directory / 'runtime'))

        for desktop in ('KDE', 'GNOME'):
            portal.desktop = desktop
            current = environment | {'XDG_CURRENT_DESKTOP': desktop, 'XDG_SESSION_DESKTOP': desktop.lower(),
                                     'QT_DEBUG_PLUGINS': '1', 'QT_MESSAGE_PATTERN': '%{category}: %{message}'}
            first = len(requests)
            code, text = run(['--selftest-native-dialogs', str(directory)], current)
            observed = requests[first:]
            expected = [('OpenFile', 'MergeDesk native open', False),
                        ('SaveFile', 'MergeDesk native save', False),
                        ('OpenFile', 'MergeDesk native folder', True)]
            passed = code == 0 and len(observed) == 3
            # Some restricted containers deny every child /proc maps read.
            # Accept only Qt's actual load event, never discovery metadata or
            # the later "unloaded library (faked)" message.
            loader_paths = sorted(set(re.findall(
                r'^qt\.core\.library: "([^"\r\n]+/libqxdgdesktopportal\.so)" loaded library\s*$',
                text, flags=re.MULTILINE)))
            for request, (method, title, is_directory) in zip(observed, expected):
                if not request['maps_readable'] and not request['portal_plugins'] and loader_paths:
                    request['portal_plugins'] = loader_paths
                    request['plugin_evidence'] = 'Qt loaded library diagnostic; process maps unavailable'
                request_passed = request['method'] == method and request['title'] == title
                request_passed = request_passed and request['parent'].startswith('x11:')
                request_passed = request_passed and bool(request['options'].get('directory', False)) == is_directory
                request_passed = request_passed and not request['options'].get('multiple', False)
                request_passed = request_passed and len(request['portal_plugins']) == 1
                if plugin_root and request['portal_plugins']:
                    request_passed = request_passed and Path(request['portal_plugins'][0]).resolve() == (
                        plugin_root / 'platformthemes/libqxdgdesktopportal.so').resolve()
                prefix = title + ': ok; selected='
                request['qt_selected_files'] = [line[len(prefix):] for line in text.splitlines()
                                                if line.startswith(prefix)]
                request['passed'] = bool(request_passed and len(request['qt_selected_files']) == 1)
                passed = passed and request['passed']
            runs.append(dict(desktop=desktop, passed=bool(passed), exit_code=code, output=text))
            print(f'{desktop}: native open/save/folder: {"ok" if passed else "FAILED"}', flush=True)
            if not passed:
                failures.append(desktop)
                print(text, flush=True)

        cases = [
            ('empty theme override', {'QT_QPA_PLATFORMTHEME': ''}, [], '1', '', 'xcb'),
            ('GTK theme override', {'QT_QPA_PLATFORMTHEME': 'gtk3'}, [], '1', 'gtk3', 'xcb'),
            ('Qt CLI theme override', {}, ['-platformtheme', 'gtk3'], '0', '', 'xcb'),
            ('offscreen platform', {'QT_QPA_PLATFORM': 'offscreen'}, [], '0', '', 'offscreen'),
            ('Qt CLI platform override', {}, ['-platform', 'minimal'], '0', '', 'minimal'),
        ]
        for name, changed, arguments, is_set, theme, platform in cases:
            current = environment | {'XDG_CURRENT_DESKTOP': 'KDE'} | changed
            code, text = run(arguments + ['--selftest-native-dialogs', 'inspect'], current)
            values = dict(line.split('=', 1) for line in text.splitlines()
                          if line.startswith(('platform-theme-set=', 'platform-theme=', 'platform=')))
            passed = code == 0 and values == {
                'platform-theme-set': is_set, 'platform-theme': theme, 'platform': platform}
            overrides.append(dict(test=name, passed=passed, observed=values, exit_code=code, output=text))
            print(f'{name}: {"ok" if passed else "FAILED"}', flush=True)
            if not passed:
                failures.append(name)
        if (directory / 'input.txt').read_text() != 'Native chooser fixture\n' or (directory / 'output.txt').exists():
            failures.append('chooser unexpectedly changed fixture files')

    result = dict(passed=not failures, executable=str(executable),
                  expected_plugin_root=str(plugin_root) if plugin_root else None,
                  dialogs_passed=sum(3 for item in runs if item['passed']),
                  overrides_passed=sum(item['passed'] for item in overrides),
                  failed=failures, runs=runs, requests=requests, overrides=overrides)
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'Native dialogs: {result["dialogs_passed"]}/6 chooser round trips, '
          f'{result["overrides_passed"]}/5 override checks, {len(failures)} failures', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
