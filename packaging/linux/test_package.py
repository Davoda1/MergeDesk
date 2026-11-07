#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Regression checks for release runtime selection and license coverage."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location('mergedesk_linux_package',
                                             Path(__file__).with_name('package.py'))
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mergedesk-package-check-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = self.root/'repository'
        self.sdk = self.root/'qt-sdk'
        self.build = self.root/'build'
        self.output = self.root/'output'
        self.notices = self.repository/'licenses/qt/6.11.3'
        for directory in [self.repository/'packaging', self.notices, self.build,
                          self.sdk/'bin', self.output]:
            directory.mkdir(parents=True)
        self.paths = {'QT_VERSION': '6.11.3', 'QT_INSTALL_PREFIX': str(self.sdk)}
        for key, directory in [('QT_INSTALL_LIBS', 'lib'), ('QT_INSTALL_PLUGINS', 'plugins'),
                               ('QT_INSTALL_TRANSLATIONS', 'translations'),
                               ('QT_INSTALL_DATA', '.'), ('QT_INSTALL_LIBEXECS', 'libexec')]:
            (self.sdk/directory).mkdir(exist_ok=True)
            self.paths[key] = str(self.sdk/directory)
        self.write_qmake()
        self.write_cache(self.sdk)
        self.write_json(self.repository/'packaging/qt-runtime.json',
                        {'version': '6.11.3', 'notices_directory': 'licenses/qt/6.11.3'})
        self.write_json(self.notices/'component-notices-manifest.json', {'qt_version': '6.11.3'})
        (self.notices/'component-notices.html').write_text('Qt component notice fixture\n')
        self.source = {'version': '6.11.3',
                       'url': 'https://download.qt.io/official_releases/qt/6.11/6.11.3/single/source.tar.xz',
                       'sha256': 'a'*64}
        self.write_json(self.notices/'source-archives.json', [self.source])
        root_patch = patch.object(package, 'ROOT', self.repository)
        root_patch.start()
        self.addCleanup(root_patch.stop)

    @staticmethod
    def write_json(path, value):
        path.write_text(json.dumps(value)+'\n')

    def write_qmake(self):
        # This executable models an SDK query, not the application under test.
        executable = self.sdk/'bin/qmake'
        text = '\n'.join(key+':'+value for key, value in self.paths.items())
        executable.write_text(f'#!{sys.executable}\nprint({text!r})\n')
        executable.chmod(0o755)

    def write_cache(self, sdk):
        self.build.joinpath('CMakeCache.txt').write_text('\n'.join(
            f'{name}_DIR:PATH={sdk}/lib/cmake/{name}'
            for name in ['Qt6', 'Qt6Core', 'Qt6WebEngineCore', 'Qt6Svg'])+'\n')

    def library(self, name, sdk=True):
        path = (self.sdk/'lib' if sdk else self.root/'system')/name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(b'library fixture')
        return path

    def icu_metadata(self):
        metadata = {'icu': {'version': '73.2', 'license_file': 'ICU-LICENSE.txt',
                            'source': {'url': 'https://example.org/icu-source.tgz',
                                       'version': '73.2', 'sha256': 'b'*64}}}
        self.write_json(self.notices/'sdk-dependencies.json', metadata)
        (self.notices/'ICU-LICENSE.txt').write_text('ICU license fixture\n')
        return metadata

    def test_explicit_sdk_does_not_select_qmake_from_path(self):
        decoy = self.root/'decoy'
        decoy.mkdir()
        (decoy/'qmake6').write_text('invalid SDK query')
        with patch.dict(os.environ, {'PATH': str(decoy)}):
            selected, notices = package.qt_sdk_configuration(self.sdk, self.build)
        self.assertEqual(selected['QT_VERSION'], '6.11.3')
        self.assertEqual(notices, self.notices)

    def test_older_runtime_is_rejected_before_packaging(self):
        self.paths['QT_VERSION'] = '6.4.2'
        self.write_qmake()
        with self.assertRaisesRegex(RuntimeError, 'require reviewed Qt'):
            package.qt_sdk_configuration(self.sdk, self.build)

    def test_application_configured_with_different_sdk_is_rejected(self):
        self.write_cache(self.root/'different-qt')
        with self.assertRaisesRegex(RuntimeError, 'not configured'):
            package.qt_sdk_configuration(self.sdk, self.build)

    def test_mixed_webengine_configuration_is_rejected(self):
        cache = self.build/'CMakeCache.txt'
        cache.write_text(cache.read_text().replace(
            f'Qt6WebEngineCore_DIR:PATH={self.sdk}',
            f'Qt6WebEngineCore_DIR:PATH={self.root}/system-qt'))
        with self.assertRaisesRegex(RuntimeError, 'Qt6WebEngineCore_DIR'):
            package.qt_sdk_configuration(self.sdk, self.build)

    def test_plugin_directory_outside_selected_sdk_is_rejected(self):
        external = self.root/'other-plugins'
        external.mkdir()
        self.paths['QT_INSTALL_PLUGINS'] = str(external)
        self.write_qmake()
        with self.assertRaisesRegex(RuntimeError, 'QT_INSTALL_PLUGINS'):
            package.qt_sdk_configuration(self.sdk, self.build)

    def test_wrong_notice_version_is_rejected(self):
        self.write_json(self.notices/'component-notices-manifest.json', {'qt_version': '6.10.3'})
        with self.assertRaisesRegex(RuntimeError, 'component notices'):
            package.qt_sdk_configuration(self.sdk, self.build)

    def test_source_record_without_digest_is_rejected(self):
        del self.source['sha256']
        self.write_json(self.notices/'source-archives.json', [self.source])
        with self.assertRaisesRegex(RuntimeError, 'source metadata'):
            package.qt_sdk_configuration(self.sdk, self.build)

    def test_unknown_sdk_library_cannot_bypass_license_collection(self):
        for name in ['libunreviewed.so.1', 'libQt6Core.so.6.4.2']:
            with self.subTest(library=name):
                library = self.library(name)
                with self.assertRaisesRegex(RuntimeError, 'Unreviewed library'):
                    package.sdk_library_component(library, self.sdk, '6.11.3', {})

    def test_system_qt_cannot_be_mixed_with_sdk(self):
        library = self.library('libQt6Core.so.6', sdk=False)
        with self.assertRaisesRegex(RuntimeError, 'system Qt library'):
            package.sdk_library_component(library, self.sdk, '6.11.3', {})

    def test_icu_version_must_match_its_own_source_record(self):
        metadata = self.icu_metadata()
        library = self.library('libicuuc.so.74.2')
        with self.assertRaisesRegex(RuntimeError, 'Unreviewed library'):
            package.sdk_library_component(library, self.sdk, '6.11.3', metadata)

    def test_sdk_and_system_libraries_keep_distinct_license_ownership(self):
        self.icu_metadata()
        qt_library = self.library('libQt6Core.so.6.11.3')
        icu_library = self.library('libicuuc.so.73.2')
        system_library = self.library('libPocoFoundation.so.80', sdk=False)
        system, sdk = package.collect_sdk_licenses(
            {qt_library, icu_library, system_library}, self.output, self.sdk, '6.11.3', self.notices)
        self.assertEqual(system, {system_library})
        self.assertEqual({entry['component'] for entry in sdk}, {'qt', 'icu'})
        self.assertTrue(all(not Path(entry['path']).is_absolute() for entry in sdk))

    def test_missing_or_mismatched_icu_license_record_is_fatal(self):
        library = self.library('libicuuc.so.73.2')
        for problem in ['missing-license', 'wrong-source-version']:
            with self.subTest(problem=problem):
                metadata = self.icu_metadata()
                if problem == 'missing-license':
                    (self.notices/'ICU-LICENSE.txt').unlink()
                else:
                    metadata['icu']['source']['version'] = '72.1'
                    self.write_json(self.notices/'sdk-dependencies.json', metadata)
                with self.assertRaisesRegex(RuntimeError, 'License or corresponding source'):
                    package.collect_sdk_licenses({library}, self.output, self.sdk, '6.11.3', self.notices)

    def test_relocation_audit_clears_developer_library_path(self):
        with patch.dict(os.environ, {'LD_LIBRARY_PATH': '/developer/sdk/lib'}), \
             patch.object(package.subprocess, 'check_output', return_value='') as command:
            list(package.dependencies(self.root/'program'))
        self.assertEqual(command.call_args.kwargs['env']['LD_LIBRARY_PATH'], '')


if __name__ == '__main__':
    unittest.main()
