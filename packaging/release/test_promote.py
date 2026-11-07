# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Offline release regressions: real archive/checksum parsing and a fake API."""
import base64
import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import stat
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import urllib.parse
import urllib.request
import warnings
import zipfile

import promote as promotion

SHA = 'a' * 40
OTHER_SHA = 'b' * 40
VERSION = '1.0.0'
QT = '6.11.3'
REPO = 'example/MergeDesk'
WINDOWS = 'MergeDesk-1.0.0-windows-x64'
LINUX = 'MergeDesk-1.0.0-linux-x86_64'


def encoded(value):
    return json.dumps(value).encode()


def test_report(windows, portable=False):
    tests = promotion.WINDOWS_TESTS if windows else promotion.LINUX_TESTS
    rows = [dict(test=name, passed=True, exit_code=0) for name in sorted(tests)]
    if windows:
        rows += [dict(test=name, language=language, passed=True, exit_code=0)
                 for language in ('en_US', 'hu', 'de', 'fr')
                 for name in ('selftest-about', 'selftest-options', 'selftest-qt-i18n')]
    result = dict(platform='windows-x64' if windows else 'linux-x86_64', tests=rows)
    if windows:
        result['portable_environment'] = portable
    return result


def dialogs():
    return dict(passed=True, failed=[], dialogs_passed=6, overrides_passed=5,
                runs=[dict(desktop=name, passed=True, exit_code=0) for name in ('KDE', 'GNOME')],
                requests=[dict(desktop=desktop, method=method, title=title, response_code=0, passed=True)
                          for desktop in ('KDE', 'GNOME') for method, title in (
                              ('OpenFile', 'MergeDesk native open'),
                              ('SaveFile', 'MergeDesk native save'),
                              ('OpenFile', 'MergeDesk native folder'))],
                overrides=[dict(test=name, passed=True, exit_code=0)
                           for name in sorted(promotion.OVERRIDE_TESTS)])


def zip_bytes(files):
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, content in files.items():
            archive.writestr(name, content)
    return output.getvalue()


def tar_bytes(files):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w:gz') as archive:
        for name, content in files.items():
            entry = tarfile.TarInfo(name)
            entry.size = len(content)
            archive.addfile(entry, io.BytesIO(content))
    return output.getvalue()


class FakeGitHub(promotion.GitHub):
    def __init__(self, directory):
        self.repository = REPO
        self.directory = directory
        self.tag_sha = SHA
        self.mutations = []
        self.releases = []
        self.uploads = []
        self.release_downloads = []
        self.upload_calls = 0
        self.fail_upload = None
        self.corrupt_release_download = False
        self.on_upload = None
        self.on_release_download = None
        self.artifacts = {}
        self.blobs = {}
        self.tool_pin = {'appimagetool': {'version': '1.9.1', 'sha256': 'c' * 64}}
        self.contents = {
            'CMakeLists.txt': b'project(MergeDesk VERSION 1.0.0 LANGUAGES C CXX)\n',
            'packaging/qt-runtime.json': encoded(dict(
                version=QT, platforms=['windows-x64', 'linux-x86_64'])),
            'packaging/linux/appimage-tools.json': encoded(self.tool_pin),
        }
        repository = dict(id=7, full_name=REPO)
        self.runs = {}
        self.jobs = {}
        jobs = {
            'build.yml': ['Windows x64 · MSVC and Qt 6.11.3',
                          'GNU/Linux x64 · Qt 6.11.3',
                          'Debian 12 baseline · Qt 6.11.3 · tar, DEB and AppImage'],
            'checks.yml': ['Workflow and secret checks', 'Cppcheck · unix64', 'Cppcheck · win64'],
        }
        for run_id, (workflow, names) in enumerate(jobs.items(), 1):
            self.runs[workflow] = [dict(
                id=run_id, run_attempt=1, head_sha=SHA, head_branch='master', event='push',
                path=f'.github/workflows/{workflow}', repository=repository,
                head_repository=repository, status='completed', conclusion='success')]
            self.jobs[run_id] = [dict(name=name, run_id=run_id, head_sha=SHA,
                                     status='completed', conclusion='success') for name in names]
        self.build_artifacts()

    def build_artifacts(self):
        records = {}
        for name, windows in ((WINDOWS, True), (LINUX, False)):
            metadata = dict(application='MergeDesk', version=VERSION, source_revision=SHA,
                            qt=QT, platform='windows-x64' if windows else 'linux-x86_64',
                            configuration='Release', runtime_linkage='dynamic')
            if not windows:
                metadata['glibc_baseline'] = 'glibc 2.36'
            report = test_report(windows, portable=windows)
            embedded = {'BUILD-METADATA.json': encoded(metadata), 'TEST-RESULTS.json': encoded(report)}
            if windows:
                embedded['BUILD-TREE-TEST-RESULTS.json'] = encoded(test_report(True))
            else:
                embedded['NATIVE-DIALOGS.json'] = encoded(dialogs())
            files = {f'{name}/{key}': value for key, value in embedded.items()}
            # PowerShell BOMs must be accepted without weakening report parsing.
            if windows:
                files[f'{name}/TEST-RESULTS.json'] = b'\xef\xbb\xbf' + files[f'{name}/TEST-RESULTS.json']
                packages = {name+'.zip': zip_bytes(embedded)}
            else:
                packages = {name+'.tar.gz': tar_bytes(embedded),
                            'mergedesk_1.0.0_amd64.deb': b'!<arch>\npackage-fixture',
                            name+'.AppImage': b'\x7fELFappimage-fixture'}
                files['APPIMAGE-BUILD-TOOLS.json'] = encoded(self.tool_pin)
                files['INSTALLED-NATIVE-DIALOGS.json'] = encoded(dialogs())
            for filename, data in packages.items():
                files[filename] = data
                files[filename+'.sha256'] = (
                    hashlib.sha256(data).hexdigest()+'  '+filename+'\r\n').encode()
            records[name] = files
        records['ubuntu-native-dialog-evidence'] = {'NATIVE-DIALOGS.json': encoded(dialogs())}
        records['repository-checks'] = {'gitleaks.json': b'[]\n'}
        for artifact_id, (name, files) in enumerate(records.items(), 11):
            self.set_artifact(name, files, artifact_id)

    def set_artifact(self, name, files, artifact_id=None):
        artifact_id = artifact_id or self.artifacts[name]['id']
        data = zip_bytes(files)
        self.blobs[artifact_id] = data
        run_id = 2 if name == 'repository-checks' else 1
        self.artifacts[name] = dict(
            id=artifact_id, name=name, size_in_bytes=len(data), expired=False,
            digest='sha256:'+hashlib.sha256(data).hexdigest(),
            workflow_run=dict(id=run_id, repository_id=7, head_repository_id=7,
                              head_branch='master', head_sha=SHA))

    def change_artifact(self, name, mutate):
        with zipfile.ZipFile(io.BytesIO(self.blobs[self.artifacts[name]['id']])) as archive:
            files = {item.filename: archive.read(item) for item in archive.infolist()}
        mutate(files)
        self.set_artifact(name, files)

    def json(self, method, path, body=None):
        parsed = urllib.parse.urlsplit(path)
        pieces = parsed.path.strip('/').split('/')
        if method != 'GET':
            self.mutations.append((method, parsed.path, copy.deepcopy(body)))
        if pieces[:3] == ['git', 'ref', 'tags']:
            result = dict(object=dict(type='commit', sha=self.tag_sha))
        elif pieces[0] == 'contents':
            filename = '/'.join(pieces[1:])
            result = dict(type='file', encoding='base64', path=filename,
                          content=base64.b64encode(self.contents[filename]).decode())
        elif pieces[:2] == ['actions', 'workflows']:
            result = dict(workflow_runs=self.runs[pieces[2]])
        elif pieces[:2] == ['actions', 'runs']:
            run_id = int(pieces[2])
            if pieces[3] == 'jobs':
                result = dict(jobs=self.jobs[run_id])
            else:
                result = dict(artifacts=[item for item in self.artifacts.values()
                                        if item['workflow_run']['id'] == run_id])
        elif pieces == ['releases']:
            if method == 'POST':
                result = dict(body, id=101)
                self.releases.append(result)
            else:
                result = self.releases
        elif pieces == ['releases', '101', 'assets']:
            result = self.uploads
        elif pieces == ['releases', '101']:
            result = self.releases[0]
            if method == 'PATCH':
                result.update(body)
        else:
            raise AssertionError((method, path))
        return copy.deepcopy(result)

    def download(self, path, destination, limit, expected_digest=None, expected_size=None, *, accept):
        pieces = path.strip('/').split('/')
        if pieces[0] == 'actions':
            if accept != 'application/vnd.github+json':
                raise promotion.ApiError(415, 'GET')
            data = self.blobs[int(pieces[2])]
        else:
            if accept != 'application/octet-stream':
                raise promotion.ApiError(415, 'GET')
            asset = next(item for item in self.uploads if item['id'] == int(pieces[2]))
            self.release_downloads.append(asset['name'])
            data = (self.directory/asset['name']).read_bytes()
            if self.on_release_download:
                self.on_release_download(self)
            if self.corrupt_release_download:
                data += b'corruption'
        promotion.require(len(data) <= limit, 'download limit')
        destination.write_bytes(data)
        actual = hashlib.sha256(data).hexdigest()
        promotion.require(expected_size is None or len(data) == expected_size, 'download size mismatch')
        promotion.require(expected_digest is None or actual == expected_digest, 'download checksum mismatch')
        return actual

    def upload(self, release_id, path):
        self.upload_calls += 1
        if self.upload_calls == self.fail_upload:
            raise promotion.ApiError(502, 'POST')
        shutil.copy2(path, self.directory/path.name)
        result = dict(id=200+self.upload_calls, name=path.name, state='uploaded',
                      size=path.stat().st_size, digest='sha256:'+promotion.sha256(path))
        self.uploads.append(result)
        if self.on_upload:
            self.on_upload(self)
        return copy.deepcopy(result)


class ReleasePromotionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mergedesk-release-test-')
        self.directory = Path(self.temporary.name)
        self.api = FakeGitHub(self.directory)
        self.stage = self.directory/'stage'
        self.stage.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def prepare(self):
        sha = promotion.resolve_tag(self.api, 'v1.0.0')
        qt = promotion.target_configuration(self.api, sha, VERSION)
        gates = promotion.wait_for_gates(self.api, sha, qt, seconds=0)
        artifacts = promotion.select_artifacts(self.api, gates, sha, VERSION)
        assets = promotion.validate_payloads(self.api, artifacts, self.stage, VERSION, sha, qt)
        return gates, assets

    def publish(self):
        gates, assets = self.prepare()
        promotion.publish_release(self.api, 'v1.0.0', SHA, QT, gates, assets, self.stage)

    def assert_blocked_before_draft(self):
        with self.assertRaises(promotion.PromotionError):
            self.publish()
        self.assertEqual([], self.api.mutations)
        self.assertEqual([], self.api.uploads)

    def test_complete_verified_release_publishes_four_packages_and_one_manifest(self):
        self.publish()
        packages = {WINDOWS+'.zip', LINUX+'.tar.gz', LINUX+'.AppImage',
                    'mergedesk_1.0.0_amd64.deb'}
        self.assertEqual(packages | {'SHA256SUMS'}, {item['name'] for item in self.api.uploads})
        self.assertEqual(5, len(self.api.uploads))
        self.assertEqual(sorted(packages | {'SHA256SUMS'}), sorted(self.api.release_downloads))
        self.assertEqual(['POST', 'PATCH'], [item[0] for item in self.api.mutations])
        self.assertFalse(self.api.releases[0]['draft'])
        self.assertNotIn('generate_release_notes', self.api.releases[0])
        self.assertIn('davoda1', self.api.releases[0]['body'])
        self.assertIn('GPL-3.0-or-later', self.api.releases[0]['body'])
        self.assertIn('SHA256SUMS', self.api.releases[0]['body'])
        self.assertEqual(SHA, self.api.releases[0]['target_commitish'])
        manifest = (self.directory/'SHA256SUMS').read_bytes()
        self.assertNotIn(b'\r', manifest)
        self.assertTrue(manifest.endswith(b'\n'))
        entries = [line.split('  ', 1) for line in manifest.decode('ascii').splitlines()]
        self.assertEqual(sorted(packages), [name for digest, name in entries])
        for digest, name in entries:
            self.assertEqual(hashlib.sha256((self.directory/name).read_bytes()).hexdigest(), digest)

    def test_missing_repository_run_does_not_create_release(self):
        self.api.runs['checks.yml'] = []
        self.assert_blocked_before_draft()

    def test_selective_dispatch_run_is_not_a_master_push_gate(self):
        self.api.runs['build.yml'][0]['event'] = 'workflow_dispatch'
        self.assert_blocked_before_draft()

    def test_workflow_path_with_master_suffix_is_accepted(self):
        for runs in self.api.runs.values():
            runs[0]['path'] += '@master'
        self.publish()
        self.assertFalse(self.api.releases[0]['draft'])

    def test_different_workflow_path_is_not_a_release_gate(self):
        self.api.runs['build.yml'][0]['path'] = '.github/workflows/unrelated.yml@master'
        self.assert_blocked_before_draft()

    def test_green_workflow_with_skipped_native_job_is_rejected(self):
        self.api.jobs[1][1]['conclusion'] = 'skipped'
        self.assert_blocked_before_draft()

    def test_missing_cppcheck_platform_is_rejected(self):
        self.api.jobs[2].pop()
        self.assert_blocked_before_draft()

    def test_failing_newer_run_does_not_fall_back_to_old_success(self):
        newer = copy.deepcopy(self.api.runs['build.yml'][0])
        newer.update(id=99, conclusion='failure')
        self.api.runs['build.yml'].append(newer)
        self.assert_blocked_before_draft()

    def test_same_sha_from_another_repository_is_not_accepted(self):
        self.api.runs['build.yml'][0]['head_repository'] = dict(id=8, full_name='different/MergeDesk')
        self.assert_blocked_before_draft()

    def test_wait_has_a_deadline(self):
        self.api.runs['build.yml'][0]['status'] = 'in_progress'
        elapsed = [0]
        def sleep(seconds):
            elapsed[0] += seconds
        with self.assertRaises(promotion.PromotionError):
            promotion.wait_for_gates(self.api, SHA, QT, seconds=2, interval=1,
                                     clock=lambda: elapsed[0], sleep=sleep)
        self.assertEqual(2, elapsed[0])

    def test_artifact_from_different_commit_is_rejected(self):
        self.api.artifacts[WINDOWS]['workflow_run']['head_sha'] = OTHER_SHA
        self.assert_blocked_before_draft()

    def test_expired_artifact_is_rejected(self):
        self.api.artifacts[WINDOWS]['expired'] = True
        self.assert_blocked_before_draft()

    def test_corrupt_download_is_rejected_before_extraction(self):
        item = self.api.artifacts[WINDOWS]
        self.api.blobs[item['id']] += b'corruption'
        self.assert_blocked_before_draft()

    def test_missing_artifact_is_rejected(self):
        del self.api.artifacts['ubuntu-native-dialog-evidence']
        self.assert_blocked_before_draft()

    def test_checksum_cannot_name_a_different_package(self):
        self.api.change_artifact(WINDOWS, lambda files: files.__setitem__(
            WINDOWS+'.zip.sha256', (('0'*64)+'  unrelated.zip\n').encode()))
        self.assert_blocked_before_draft()

    def test_checksum_must_match_package_bytes(self):
        self.api.change_artifact(LINUX, lambda files: files.__setitem__(
            LINUX+'.AppImage', b'\x7fELFmodified-package'))
        self.assert_blocked_before_draft()

    def test_metadata_must_match_exact_source_commit(self):
        def change(files):
            value = json.loads(files[f'{WINDOWS}/BUILD-METADATA.json'])
            value['source_revision'] = OTHER_SHA
            files[f'{WINDOWS}/BUILD-METADATA.json'] = encoded(value)
        self.api.change_artifact(WINDOWS, change)
        self.assert_blocked_before_draft()

    def test_tag_version_must_match_project_version(self):
        self.api.contents['CMakeLists.txt'] = b'project(MergeDesk VERSION 2.0.0 LANGUAGES C CXX)'
        self.assert_blocked_before_draft()

    def test_failed_test_cannot_be_hidden_by_passed_flag(self):
        def change(files):
            value = json.loads(files[f'{LINUX}/TEST-RESULTS.json'])
            value['tests'][0]['exit_code'] = 1
            files[f'{LINUX}/TEST-RESULTS.json'] = encoded(value)
        self.api.change_artifact(LINUX, change)
        self.assert_blocked_before_draft()

    def test_missing_web_test_is_rejected(self):
        report = test_report(False)
        report['tests'] = [row for row in report['tests'] if row['test'] != 'selftest-web']
        with self.assertRaises(promotion.PromotionError):
            promotion.validate_report(report, 'linux-x86_64')

    def test_missing_locale_test_is_rejected(self):
        report = test_report(True, True)
        report['tests'] = [row for row in report['tests']
                           if (row['test'], row.get('language')) != ('selftest-qt-i18n', 'hu')]
        with self.assertRaises(promotion.PromotionError):
            promotion.validate_report(report, 'windows-x64', True)

    def test_duplicate_test_cannot_stand_in_for_missing_test(self):
        report = test_report(False)
        report['tests'].append(copy.deepcopy(report['tests'][0]))
        with self.assertRaises(promotion.PromotionError):
            promotion.validate_report(report, 'linux-x86_64')

    def test_native_dialog_nonzero_exit_is_rejected(self):
        report = dialogs()
        report['runs'][0]['exit_code'] = 1
        with self.assertRaises(promotion.PromotionError):
            promotion.validate_dialogs(report)

    def test_native_dialog_duplicate_chooser_is_rejected(self):
        report = dialogs()
        report['requests'][-1] = copy.deepcopy(report['requests'][0])
        with self.assertRaises(promotion.PromotionError):
            promotion.validate_dialogs(report)

    def test_secret_findings_block_all_release_mutations(self):
        self.api.change_artifact('repository-checks', lambda files: files.__setitem__(
            'gitleaks.json', encoded([{'Description': 'test finding, no credential'}])))
        self.assert_blocked_before_draft()

    def test_existing_release_is_never_overwritten(self):
        self.api.releases = [dict(id=90, tag_name='v1.0.0', draft=False)]
        self.assert_blocked_before_draft()

    def test_upload_failure_leaves_draft_unpublished(self):
        self.api.fail_upload = 2
        with self.assertRaises(promotion.PromotionError):
            self.publish()
        self.assertTrue(self.api.releases[0]['draft'])
        self.assertEqual(['POST'], [item[0] for item in self.api.mutations])

    def test_release_download_corruption_prevents_publication(self):
        self.api.corrupt_release_download = True
        with self.assertRaises(promotion.PromotionError):
            self.publish()
        self.assertTrue(self.api.releases[0]['draft'])
        self.assertNotIn('PATCH', [item[0] for item in self.api.mutations])

    def test_retag_during_upload_prevents_publication(self):
        self.api.on_upload = lambda api: setattr(api, 'tag_sha', OTHER_SHA)
        with self.assertRaises(promotion.PromotionError):
            self.publish()
        self.assertTrue(self.api.releases[0]['draft'])

    def test_restarted_ci_during_upload_prevents_publication(self):
        self.api.on_upload = lambda api: api.runs['build.yml'][0].update(
            run_attempt=2, status='in_progress')
        with self.assertRaises(promotion.PromotionError):
            self.publish()
        self.assertTrue(self.api.releases[0]['draft'])

    def test_added_asset_after_verification_prevents_publication(self):
        def add(api):
            if not any(item['name'] == 'unexpected.txt' for item in api.uploads):
                api.uploads.append(dict(id=999, name='unexpected.txt', size=1, state='uploaded'))
        self.api.on_release_download = add
        with self.assertRaises(promotion.PromotionError):
            self.publish()
        self.assertTrue(self.api.releases[0]['draft'])

    def test_changed_notes_after_verification_prevent_publication(self):
        self.api.on_release_download = lambda api: api.releases[0].update(body='changed notes')
        with self.assertRaises(promotion.PromotionError):
            self.publish()
        self.assertTrue(self.api.releases[0]['draft'])

    def test_unsafe_tag_text_is_data_not_a_command(self):
        for tag in ('v1.0.0;echo unsafe', '../v1.0.0', 'v1.0.0\nextra', 'v01.0.0'):
            with self.subTest(tag=tag), self.assertRaises(promotion.PromotionError):
                promotion.tag_version(tag)

    def test_repository_dot_path_components_are_rejected(self):
        for repository in ('../MergeDesk', './MergeDesk', 'example/..', 'example/.'):
            with self.subTest(repository=repository), self.assertRaises(promotion.PromotionError):
                promotion.GitHub(repository, 'test-only')

    def test_artifact_traversal_and_unexpected_members_are_rejected(self):
        for filename in ('../escaped', '/absolute', 'folder\\escape', 'unrequested.log'):
            with self.subTest(filename=filename):
                archive = self.directory/'malicious.zip'
                archive.write_bytes(zip_bytes({'expected.json': b'{}', filename: b'bad'}))
                with self.assertRaises(promotion.PromotionError):
                    promotion.unpack_artifact(archive, self.directory/'extract', {'expected.json'})
                self.assertFalse((self.directory/'extract').exists())

    def test_artifact_symlink_and_duplicate_paths_are_rejected(self):
        for symlink in (True, False):
            archive = self.directory/'malicious.zip'
            with warnings.catch_warnings(), zipfile.ZipFile(archive, 'w') as bundle:
                warnings.simplefilter('ignore', UserWarning)
                entry = zipfile.ZipInfo('expected.json')
                if symlink:
                    entry.create_system = 3
                    entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                bundle.writestr(entry, b'{}')
                if not symlink:
                    bundle.writestr('expected.json', b'{}')
            with self.assertRaises(promotion.PromotionError):
                promotion.unpack_artifact(archive, self.directory/'extract', {'expected.json'})
            self.assertFalse((self.directory/'extract').exists())

    def test_embedded_report_symlink_is_not_a_regular_report(self):
        archive = self.directory/'embedded.zip'
        with zipfile.ZipFile(archive, 'w') as bundle:
            entry = zipfile.ZipInfo('BUILD-METADATA.json')
            entry.create_system = 3
            entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            bundle.writestr(entry, b'{}')
        with self.assertRaises(promotion.PromotionError):
            promotion.inner_reports(archive, {'BUILD-METADATA.json'}, True)

    def test_expanded_archive_limit_is_checked_before_writing(self):
        archive = self.directory/'large.zip'
        archive.write_bytes(zip_bytes({'expected.json': b'x' * 100}))
        with patch.object(promotion, 'MAX_ARTIFACT', 20), self.assertRaises(promotion.PromotionError):
            promotion.unpack_artifact(archive, self.directory/'extract', {'expected.json'})
        self.assertFalse((self.directory/'extract').exists())

    def test_download_redirect_strips_bearer_and_rejects_unsafe_hosts(self):
        handler = promotion.DownloadRedirect()
        request = urllib.request.Request('https://api.github.com/repos/example/MergeDesk/actions/artifacts/1/zip',
                                         headers={'Authorization': 'Bearer test-only'})
        redirected = handler.redirect_request(request, None, 302, 'Found', {},
                                               'https://release-assets.githubusercontent.com/asset')
        self.assertFalse(redirected.has_header('Authorization'))
        for target in ('http://release-assets.githubusercontent.com/asset',
                       'https://example.invalid/asset', 'https://api.github.com@evil.invalid/asset'):
            with self.subTest(target=target), self.assertRaises(promotion.PromotionError):
                handler.redirect_request(request, None, 302, 'Found', {}, target)

    def test_download_sends_the_endpoint_specific_accept_header(self):
        client = promotion.GitHub(REPO, 'test-only')
        for index, (endpoint, media_type) in enumerate((
                ('/actions/artifacts/11/zip', 'application/vnd.github+json'),
                ('/releases/assets/201', 'application/octet-stream'))):
            with self.subTest(endpoint=endpoint), patch.object(
                    client, '_open', return_value=io.BytesIO(b'fixture')) as opened:
                destination = self.directory/f'http-download-{index}'
                client.download(endpoint, destination, 100, accept=media_type)
                self.assertEqual(media_type, opened.call_args.args[0].get_header('Accept'))
                self.assertEqual(b'fixture', destination.read_bytes())


if __name__ == '__main__':
    unittest.main()
