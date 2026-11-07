#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Promote exact-commit, fully tested Actions artifacts to a verified release."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

MIB = 1024 * 1024
MAX_JSON = 16 * MIB
MAX_ASSET = 2 * 1024 * MIB
MAX_ARTIFACT = 6 * 1024 * MIB
WINDOWS_TESTS = {
    'selftest-merge', 'selftest-save', 'selftest-undo', 'selftest-nav', 'selftest-copy',
    'selftest-command-line', 'selftest-codepage', 'selftest-eol', 'selftest-table-edit',
    'selftest-table-3way', 'selftest-table-options', 'selftest-image-options',
    'selftest-tree-sitter', 'selftest-subfolders', 'selftest-folder3',
    'selftest-folder-content', 'selftest-compare-methods', 'selftest-hex',
    'selftest-hex-edit', 'selftest-archive', 'selftest-archive-mixed',
    'selftest-find-replace', 'selftest-reload', 'selftest-filters', 'selftest-theme',
    'selftest-safety', 'selftest-mergedesk-features', 'selftest-table',
    'selftest-image', 'selftest-merge3', 'selftest-web',
}
LINUX_TESTS = {
    'selftest-safety', 'selftest-mergedesk-features', 'selftest-theme',
    'selftest-codepage', 'selftest-table-edit', 'selftest-image-options',
    'selftest-tree-sitter', 'selftest-hex-edit', 'selftest-archive',
    'selftest-folder3', 'selftest-subfolders', 'selftest-web', 'native-file-dialogs',
}
OVERRIDE_TESTS = {
    'empty theme override', 'GTK theme override', 'Qt CLI theme override',
    'offscreen platform', 'Qt CLI platform override',
}


class PromotionError(RuntimeError):
    pass


class ApiError(PromotionError):
    def __init__(self, status: int, method: str):
        super().__init__(f'GitHub returned HTTP {status} for {method}; no response body or signed URL is logged')
        self.status = status


def require(condition, message):
    if not condition:
        raise PromotionError(message)


def object_id(value):
    require(type(value) is int and value > 0, 'GitHub returned an invalid object ID')
    return value


def commit_id(value):
    require(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{40}', value),
            'Expected a complete lowercase Git commit SHA')
    return value


def tag_version(tag):
    require(re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', tag),
            'Release tag must be a stable version such as v1.0.0')
    return tag[1:]


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(MIB), b''):
            digest.update(block)
    return digest.hexdigest()


def read_json(path):
    require(path.stat().st_size <= MAX_JSON, f'JSON report is too large: {path.name}')
    return json.loads(path.read_text(encoding='utf-8-sig'))


class DownloadRedirect(urllib.request.HTTPRedirectHandler):
    """Follow GitHub storage redirects without forwarding API credentials."""
    def redirect_request(self, request, response, code, message, headers, new_url):
        destination = urllib.parse.urlsplit(new_url)
        host = destination.hostname or ''
        require(destination.scheme == 'https' and not destination.username
                and not destination.password and destination.port in (None, 443),
                'Refused a non-HTTPS download redirect')
        require(host == 'api.github.com' or any(host.endswith(suffix) for suffix in (
            '.githubusercontent.com', '.blob.core.windows.net', '.amazonaws.com')),
            'Refused a download redirect outside GitHub artifact storage')
        require(request.get_method() in ('GET', 'HEAD'),
                'Refused a redirect for a release mutation')
        redirected = super().redirect_request(request, response, code, message, headers, new_url)
        if redirected is not None and destination.netloc != urllib.parse.urlsplit(request.full_url).netloc:
            redirected.remove_header('Authorization')
        return redirected


class GitHub:
    """Uses the workflow-provided GITHUB_TOKEN; no separate credential is required."""
    def __init__(self, repository, token):
        require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository)
                and all(part not in ('.', '..') for part in repository.split('/')),
                'Invalid GitHub repository')
        require(token, 'GITHUB_TOKEN is required')
        self.repository = repository
        self.base = f'/repos/{repository}'
        self.token = token
        self.opener = urllib.request.build_opener(DownloadRedirect())

    def _request(self, method, path, data=None, accept='application/vnd.github+json',
                 upload=False, content_type='application/json', size=None):
        require(path.startswith('/') and not path.startswith('//') and '..' not in path,
                'Invalid repository API path')
        host = 'uploads.github.com' if upload else 'api.github.com'
        headers = {'Authorization': f'Bearer {self.token}', 'Accept': accept,
                   'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'MergeDesk-release'}
        if data is not None:
            headers['Content-Type'] = content_type
        if size is not None:
            headers['Content-Length'] = str(size)
        return urllib.request.Request(f'https://{host}{self.base}{path}',
                                      data=data, headers=headers, method=method)

    def _open(self, request):
        try:
            return self.opener.open(request, timeout=90)
        except urllib.error.HTTPError as error:
            raise ApiError(error.code, request.get_method()) from None
        except (urllib.error.URLError, TimeoutError):
            raise PromotionError('GitHub transport failed; no URL or credential is logged') from None

    def json(self, method, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        with self._open(self._request(method, path, data)) as response:
            result = response.read(MAX_JSON + 1)
        require(len(result) <= MAX_JSON, 'GitHub JSON response exceeded the limit')
        return json.loads(result)

    def pages(self, path, key=None):
        result = []
        for page in range(1, 11):
            separator = '&' if '?' in path else '?'
            payload = self.json('GET', f'{path}{separator}per_page=100&page={page}')
            items = payload[key] if key else payload
            require(isinstance(items, list), 'GitHub returned an invalid paginated response')
            result.extend(items)
            if len(items) < 100:
                return result
        raise PromotionError('GitHub pagination exceeded the bounded review limit')

    def download(self, path, destination, limit, expected_digest=None, expected_size=None, *, accept):
        count = 0
        digest = hashlib.sha256()
        with self._open(self._request('GET', path, accept=accept)) as response:
            with destination.open('xb') as output:
                for block in iter(lambda: response.read(MIB), b''):
                    count += len(block)
                    require(count <= limit, 'Downloaded artifact exceeded the size limit')
                    digest.update(block)
                    output.write(block)
        if expected_size is not None:
            require(count == expected_size, 'Downloaded artifact size differs from GitHub metadata')
        if expected_digest is not None:
            require(digest.hexdigest() == expected_digest, 'Downloaded artifact SHA-256 mismatch')
        return digest.hexdigest()

    def upload(self, release_id, path):
        endpoint = f'/releases/{object_id(release_id)}/assets?{urllib.parse.urlencode({"name": path.name})}'
        with path.open('rb') as stream:
            request = self._request('POST', endpoint, stream, upload=True,
                                    content_type='application/octet-stream', size=path.stat().st_size)
            with self._open(request) as response:
                data = response.read(MAX_JSON + 1)
        require(len(data) <= MAX_JSON, 'Release upload response exceeded the size limit')
        return json.loads(data)


def resolve_tag(api, tag):
    tag_version(tag)
    reference = api.json('GET', f'/git/ref/tags/{urllib.parse.quote(tag, safe="")}')
    item = reference['object']
    for _ in range(6):
        sha = commit_id(item.get('sha'))
        if item.get('type') == 'commit':
            return sha
        require(item.get('type') == 'tag', 'Release tag does not reference a commit')
        item = api.json('GET', f'/git/tags/{sha}')['object']
    raise PromotionError('Annotated release tag nesting exceeded the limit')


def target_file(api, sha, path):
    document = api.json('GET', f'/contents/{path}?ref={commit_id(sha)}')
    require(document.get('type') == 'file' and document.get('encoding') == 'base64'
            and document.get('path') == path, 'Tagged source metadata is not the expected file')
    content = base64.b64decode(document['content'], validate=False)
    require(len(content) <= MAX_JSON, 'Tagged source metadata exceeded the size limit')
    return content


def target_configuration(api, sha, version):
    cmake = target_file(api, sha, 'CMakeLists.txt').decode()
    require(re.search(r'project\s*\(\s*MergeDesk\s+VERSION\s+' + re.escape(version) + r'\s', cmake),
            'Tag version does not match the tagged CMake project version')
    runtime = json.loads(target_file(api, sha, 'packaging/qt-runtime.json'))
    qt = runtime.get('version', '')
    require(re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', qt), 'Tagged Qt runtime pin is invalid')
    require(set(runtime.get('platforms', [])) == {'windows-x64', 'linux-x86_64'},
            'Tagged Qt runtime does not cover both release platforms')
    return qt


def expected_jobs(qt):
    return {
        'build.yml': {
            f'Windows x64 · MSVC and Qt {qt}',
            f'GNU/Linux x64 · Qt {qt}',
            f'Debian 12 baseline · Qt {qt} · tar, DEB and AppImage',
        },
        'checks.yml': {'Workflow and secret checks', 'Cppcheck · unix64', 'Cppcheck · win64'},
    }


def check_gates(api, sha, qt):
    selected = {}
    pending = False
    for workflow, names in expected_jobs(qt).items():
        query = urllib.parse.urlencode({'head_sha': sha, 'branch': 'master', 'event': 'push'})
        runs = api.pages(f'/actions/workflows/{workflow}/runs?{query}', 'workflow_runs')
        matching = [run for run in runs if (
            run.get('head_sha') == sha and run.get('head_branch') == 'master'
            and run.get('event') == 'push'
            and run.get('path', '').split('@', 1)[0] == f'.github/workflows/{workflow}'
            and run.get('repository', {}).get('full_name', '').casefold() == api.repository.casefold()
            and run.get('head_repository', {}).get('full_name', '').casefold() == api.repository.casefold())]
        if not matching:
            pending = True
            continue
        # A newer failing run must never be hidden by an older green run.
        run = max(matching, key=lambda item: object_id(item['id']))
        if run.get('status') != 'completed':
            pending = True
            continue
        require(run.get('conclusion') == 'success', f'{workflow} latest exact-commit run did not pass')
        jobs = api.pages(f'/actions/runs/{object_id(run["id"])}/jobs?filter=latest', 'jobs')
        require(len(jobs) == len(names) and {job.get('name') for job in jobs} == names,
                f'{workflow} did not run every required job exactly once')
        require(all(job.get('status') == 'completed' and job.get('conclusion') == 'success'
                    and job.get('run_id') == run['id'] and job.get('head_sha') == sha for job in jobs),
                f'{workflow} contains a skipped, failed, incomplete, or mismatched job')
        selected[workflow] = run
    return None if pending else selected


def wait_for_gates(api, sha, qt, seconds=7200, interval=20, clock=time.monotonic, sleep=time.sleep):
    deadline = clock() + seconds
    while True:
        gates = check_gates(api, sha, qt)
        if gates is not None:
            return gates
        remaining = deadline - clock()
        require(remaining > 0, 'Required exact-commit master push gates are missing or still running')
        print('Waiting for the exact-commit native and repository gates...', flush=True)
        sleep(min(interval, remaining))


def artifact_layout(version):
    windows = f'MergeDesk-{version}-windows-x64'
    linux = f'MergeDesk-{version}-linux-x86_64'
    packages = [f'{windows}.zip', f'{linux}.tar.gz',
                f'mergedesk_{version}_amd64.deb', f'{linux}.AppImage']
    return {
        windows: {packages[0], packages[0]+'.sha256',
                  f'{windows}/BUILD-METADATA.json', f'{windows}/TEST-RESULTS.json',
                  f'{windows}/BUILD-TREE-TEST-RESULTS.json'},
        linux: set(packages[1:] + [name+'.sha256' for name in packages[1:]]) | {
            'APPIMAGE-BUILD-TOOLS.json', 'INSTALLED-NATIVE-DIALOGS.json',
            f'{linux}/BUILD-METADATA.json', f'{linux}/TEST-RESULTS.json',
            f'{linux}/NATIVE-DIALOGS.json'},
        'ubuntu-native-dialog-evidence': {'NATIVE-DIALOGS.json'},
        'repository-checks': {'gitleaks.json'},
    }, packages


def select_artifacts(api, gates, sha, version):
    layout, _ = artifact_layout(version)
    result = {}
    for workflow, run in gates.items():
        artifacts = api.pages(f'/actions/runs/{object_id(run["id"])}/artifacts', 'artifacts')
        wanted = {'repository-checks'} if workflow == 'checks.yml' else set(layout) - {'repository-checks'}
        for name in wanted:
            matches = [item for item in artifacts if item.get('name') == name]
            require(len(matches) == 1, f'Required artifact is missing or duplicated: {name}')
            item = matches[0]
            provenance = item.get('workflow_run', {})
            require(item.get('expired') is False and 0 < item.get('size_in_bytes', 0) <= MAX_ARTIFACT,
                    f'Artifact is expired, empty, or oversized: {name}')
            require(provenance.get('id') == run['id'] and provenance.get('head_sha') == sha
                    and provenance.get('head_branch') == 'master'
                    and provenance.get('repository_id') == run['repository']['id']
                    and provenance.get('head_repository_id') == run['repository']['id'],
                    f'Artifact provenance does not match the tested commit: {name}')
            require(re.fullmatch(r'sha256:[0-9a-f]{64}', item.get('digest', '')),
                    f'Artifact has no GitHub SHA-256 digest: {name}')
            object_id(item['id'])
            result[name] = item
    require(set(result) == set(layout), 'Not every required artifact was selected')
    return result


def unpack_artifact(archive, destination, expected):
    """Read only an exact allowlist; never extract archive permissions or links."""
    with zipfile.ZipFile(archive) as bundle:
        entries = bundle.infolist()
        require(len(entries) <= 1000, 'Artifact contains too many entries')
        names = set()
        files = {}
        total = 0
        parents = {str(parent) for name in expected for parent in PurePosixPath(name).parents if str(parent) != '.'}
        for item in entries:
            name = item.filename
            path = PurePosixPath(name.rstrip('/'))
            require(name and '\\' not in name and ':' not in name and '\0' not in name
                    and not path.is_absolute() and '..' not in path.parts
                    and str(path) == name.rstrip('/'), 'Unsafe path in artifact archive')
            require(str(path) not in names, 'Duplicate path in artifact archive')
            names.add(str(path))
            kind = stat.S_IFMT(item.external_attr >> 16)
            require(kind in (0, stat.S_IFREG, stat.S_IFDIR), 'Artifact contains a link or special file')
            if item.is_dir():
                require(str(path) in parents, 'Unexpected directory in artifact archive')
                continue
            require(kind != stat.S_IFDIR and name in expected, 'Unexpected file in artifact archive')
            require(item.file_size <= MAX_ASSET, 'Artifact entry exceeds the release asset limit')
            total += item.file_size
            require(total <= MAX_ARTIFACT, 'Expanded artifact exceeds the size limit')
            files[name] = item
        require(set(files) == set(expected), 'Artifact is missing a required package or report')
        destination.mkdir(parents=True, exist_ok=False)
        for name, item in files.items():
            target = destination / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(item) as source, target.open('xb') as output:
                shutil.copyfileobj(source, output, length=MIB)
            require(target.stat().st_size == item.file_size, 'Artifact entry size mismatch')


def validate_report(report, platform, portable=None):
    require(report.get('platform') == platform, 'Test report platform mismatch')
    if portable is not None:
        require(report.get('portable_environment') is portable, 'Windows test environment was not verified')
    rows = report.get('tests')
    require(isinstance(rows, list) and rows, 'Test report is empty')
    seen = set()
    for row in rows:
        require(isinstance(row, dict) and row.get('passed') is True
                and type(row.get('exit_code')) is int and row['exit_code'] == 0,
                'Test report contains a failed or malformed result')
        key = (row.get('test'), row.get('language', ''))
        require(isinstance(key[0], str) and isinstance(key[1], str) and key not in seen,
                'Test report has duplicate or malformed test identities')
        seen.add(key)
    required = {(name, '') for name in (WINDOWS_TESTS if platform == 'windows-x64' else LINUX_TESTS)}
    if platform == 'windows-x64':
        required |= {(name, language) for name in (
            'selftest-about', 'selftest-options', 'selftest-qt-i18n')
            for language in ('en_US', 'hu', 'de', 'fr')}
    require(required <= seen, 'Test report omits required tests or localizations')


def validate_dialogs(report):
    require(report.get('passed') is True and report.get('failed') == []
            and report.get('dialogs_passed') == 6 and report.get('overrides_passed') == 5,
            'Native dialog report is incomplete or failed')
    runs, requests, overrides = (report.get(name, []) for name in ('runs', 'requests', 'overrides'))
    expected_requests = {(desktop, method, title) for desktop in ('KDE', 'GNOME')
                         for method, title in (
                             ('OpenFile', 'MergeDesk native open'),
                             ('SaveFile', 'MergeDesk native save'),
                             ('OpenFile', 'MergeDesk native folder'))}
    require(len(runs) == 2 and {row.get('desktop') for row in runs} == {'KDE', 'GNOME'}
            and len(requests) == 6 and len(overrides) == 5
            and {(row.get('desktop'), row.get('method'), row.get('title')) for row in requests} == expected_requests
            and all(type(row.get('response_code')) is int and row['response_code'] == 0 for row in requests)
            and {row.get('test') for row in overrides} == OVERRIDE_TESTS
            and all(row.get('passed') is True for row in runs + requests + overrides)
            and all(type(row.get('exit_code')) is int and row['exit_code'] == 0
                    for row in runs + overrides),
            'Native dialog report omits desktop, chooser, or override checks')


def validate_metadata(metadata, platform, version, sha, qt):
    require(all(metadata.get(key) == value for key, value in {
        'application': 'MergeDesk', 'version': version, 'source_revision': sha,
        'qt': qt, 'platform': platform, 'configuration': 'Release',
        'runtime_linkage': 'dynamic',
    }.items()), 'Package version, source commit, platform, or Qt metadata mismatch')
    if platform == 'linux-x86_64':
        require(metadata.get('glibc_baseline') == 'glibc 2.36', 'Linux package is not the Debian 12 baseline')


def inner_reports(path, expected, windows):
    """Inspect data only; no packaged executable or installer is run."""
    result = {}
    if windows:
        with zipfile.ZipFile(path) as archive:
            for name in expected:
                matches = [entry for entry in archive.infolist() if entry.filename == name]
                require(len(matches) == 1 and not matches[0].is_dir()
                        and stat.S_IFMT(matches[0].external_attr >> 16) in (0, stat.S_IFREG)
                        and matches[0].file_size <= MAX_JSON,
                        'Windows package has missing, duplicate, or oversized embedded reports')
                result[name] = json.loads(archive.read(matches[0]).decode('utf-8-sig'))
    else:
        expanded = 0
        with tarfile.open(path, 'r|gz') as archive:
            for entry in archive:
                expanded += entry.size
                require(expanded <= 12 * 1024 * MIB, 'Linux package expanded size exceeds the limit')
                if entry.name not in expected:
                    continue
                require(entry.isfile() and entry.name not in result and entry.size <= MAX_JSON,
                        'Linux package has duplicate, nonregular, or oversized embedded reports')
                result[entry.name] = json.loads(archive.extractfile(entry).read().decode('utf-8-sig'))
    require(set(result) == set(expected), 'Package omits embedded build or test reports')
    return result


def validate_payloads(api, artifacts, destination, version, sha, qt):
    layout, packages = artifact_layout(version)
    directories = {}
    for name, expected in layout.items():
        item = artifacts[name]
        archive = destination / f'artifact-{item["id"]}.zip'
        # Actions returns its archive redirect through the JSON API endpoint;
        # release assets use a separate binary-content media type below.
        api.download(f'/actions/artifacts/{item["id"]}/zip', archive, MAX_ARTIFACT,
                     item['digest'].split(':', 1)[1], item['size_in_bytes'],
                     accept='application/vnd.github+json')
        directory = destination / name
        unpack_artifact(archive, directory, expected)
        archive.unlink()
        directories[name] = directory
    windows = f'MergeDesk-{version}-windows-x64'
    linux = f'MergeDesk-{version}-linux-x86_64'
    for name, platform in ((windows, 'windows-x64'), (linux, 'linux-x86_64')):
        base = directories[name] / name
        metadata = read_json(base/'BUILD-METADATA.json')
        validate_metadata(metadata, platform, version, sha, qt)
        report = read_json(base/'TEST-RESULTS.json')
        validate_report(report, platform, True if platform == 'windows-x64' else None)
        embedded = {'BUILD-METADATA.json': metadata, 'TEST-RESULTS.json': report}
        if platform == 'windows-x64':
            build_report = read_json(base/'BUILD-TREE-TEST-RESULTS.json')
            validate_report(build_report, platform, False)
            embedded['BUILD-TREE-TEST-RESULTS.json'] = build_report
        else:
            dialogs = read_json(base/'NATIVE-DIALOGS.json')
            validate_dialogs(dialogs)
            validate_dialogs(read_json(directories[name]/'INSTALLED-NATIVE-DIALOGS.json'))
            embedded['NATIVE-DIALOGS.json'] = dialogs
        package = directories[name]/(packages[0] if platform == 'windows-x64' else packages[1])
        require(inner_reports(package, embedded, platform == 'windows-x64') == embedded,
                'Embedded package reports differ from the tested artifact reports')
    validate_dialogs(read_json(directories['ubuntu-native-dialog-evidence']/'NATIVE-DIALOGS.json'))
    require(read_json(directories['repository-checks']/'gitleaks.json') == [],
            'Secret scan report is not an empty findings list')
    tool_pin = json.loads(target_file(api, sha, 'packaging/linux/appimage-tools.json'))
    require(read_json(directories[linux]/'APPIMAGE-BUILD-TOOLS.json') == tool_pin,
            'AppImage build tools differ from the tagged pins')
    assets = {}
    for name in packages:
        path = directories[windows if name == packages[0] else linux] / name
        sidecar = path.with_name(name+'.sha256')
        require(sidecar.stat().st_size <= 512, 'Checksum sidecar is oversized')
        match = re.fullmatch(r'([0-9a-fA-F]{64}) [ *]' + re.escape(name) + r'\r?\n?',
                            sidecar.read_text(encoding='ascii'))
        digest = sha256(path)
        require(match and match.group(1).lower() == digest, 'Package checksum or checksum filename mismatch')
        with path.open('rb') as stream:
            magic = stream.read(8)
        if name.endswith('.deb'):
            require(magic == b'!<arch>\n', 'DEB package format is invalid')
        if name.endswith('.AppImage'):
            require(magic[:4] == b'\x7fELF', 'AppImage package format is invalid')
        assets[name] = {'path': path, 'sha256': digest, 'size': path.stat().st_size}
    manifest = destination / 'SHA256SUMS'
    manifest.write_bytes(''.join(f'{assets[name]["sha256"]}  {name}\n'
                                for name in sorted(assets)).encode('ascii'))
    assets[manifest.name] = {'path': manifest, 'sha256': sha256(manifest),
                            'size': manifest.stat().st_size}
    return assets


def recheck(api, tag, sha, qt, original):
    require(resolve_tag(api, tag) == sha, 'Release tag moved during verification')
    current = check_gates(api, sha, qt)
    require(current is not None and {key: (run['id'], run.get('run_attempt')) for key, run in current.items()}
            == {key: (run['id'], run.get('run_attempt')) for key, run in original.items()},
            'Required CI runs changed or restarted during verification; rerun promotion')


def publish_release(api, tag, sha, qt, gates, assets, directory):
    recheck(api, tag, sha, qt, gates)
    existing = [release for release in api.pages('/releases') if release.get('tag_name') == tag]
    require(not existing, 'A release already exists for this tag; inspect any unfinished draft before rerunning')
    body = (
        f'MergeDesk {tag_version(tag)} provides native Qt6 comparison and merging on Windows and GNU/Linux.\n\n'
        'Downloads include a Windows portable ZIP and GNU/Linux tar, DEB, and AppImage packages. '
        'A single SHA256SUMS manifest covers all four packages. '
        'License notices and build information are included in the packages.\n\n'
        'Copyright (C) 2026 davoda1. MergeDesk is licensed under GPL-3.0-or-later; '
        'bundled third-party components retain their documented licenses.\n\n'
        f'Tested source commit: {sha}\n\n'
        'All native build and repository checks passed for this exact commit before promotion. '
        'The release assets were downloaded again and their hashes verified before publication.'
    )
    draft = api.json('POST', '/releases', {
        'tag_name': tag, 'target_commitish': sha, 'name': f'MergeDesk {tag_version(tag)}',
        'body': body, 'draft': True, 'prerelease': False,
    })
    release_id = object_id(draft.get('id'))
    require(draft.get('draft') is True and draft.get('tag_name') == tag,
            'GitHub did not create the requested draft release')
    print(f'Created draft release {tag}; uploading {len(assets)} verified assets.', flush=True)
    for name, asset in sorted(assets.items()):
        uploaded = api.upload(release_id, asset['path'])
        require(uploaded.get('name') == name and uploaded.get('state') == 'uploaded'
                and uploaded.get('size') == asset['size'], 'Release asset upload metadata mismatch')
        object_id(uploaded.get('id'))
    uploaded = api.pages(f'/releases/{release_id}/assets')
    require(len(uploaded) == len(assets) and {item.get('name') for item in uploaded} == set(assets),
            'Draft release asset set differs from the verified package set')
    for item in uploaded:
        asset = assets[item['name']]
        require(item.get('state') == 'uploaded' and item.get('size') == asset['size'],
                'Draft release contains an incomplete asset')
        require(item.get('digest') in (None, 'sha256:'+asset['sha256']),
                'GitHub release asset digest mismatch')
        downloaded = directory / f'release-asset-{object_id(item["id"])}'
        api.download(f'/releases/assets/{item["id"]}', downloaded, MAX_ASSET,
                     asset['sha256'], asset['size'], accept='application/octet-stream')
        downloaded.unlink()
    # Uploads can take time: check the tag and latest complete CI again here.
    recheck(api, tag, sha, qt, gates)
    def asset_identity(item):
        return (item.get('id'), item.get('name'), item.get('size'),
                item.get('state'), item.get('digest'))
    final_assets = api.pages(f'/releases/{release_id}/assets')
    require(len(final_assets) == len(uploaded)
            and {asset_identity(item) for item in final_assets} == {asset_identity(item) for item in uploaded},
            'Draft release assets changed after download verification')
    current = api.json('GET', f'/releases/{release_id}')
    require(current.get('draft') is True and current.get('tag_name') == tag
            and current.get('name') == f'MergeDesk {tag_version(tag)}'
            and current.get('body') == body and current.get('prerelease') is False,
            'Draft release changed during upload verification')
    published = api.json('PATCH', f'/releases/{release_id}', {'draft': False, 'make_latest': 'true'})
    require(published.get('draft') is False and published.get('tag_name') == tag
            and published.get('id') == release_id, 'GitHub did not confirm publication')
    print(f'Published verified release {tag}.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=['verify', 'publish'])
    parser.add_argument('--tag', required=True)
    parser.add_argument('--expected-sha')
    parser.add_argument('--wait-seconds', type=int, default=7200)
    args = parser.parse_args()
    require(0 <= args.wait_seconds <= 7200, 'Wait limit must be between 0 and 7200 seconds')
    api = GitHub(os.environ.get('GITHUB_REPOSITORY', ''), os.environ.get('GITHUB_TOKEN', ''))
    version = tag_version(args.tag)
    sha = resolve_tag(api, args.tag)
    if args.expected_sha:
        require(sha == commit_id(args.expected_sha), 'Release tag differs from the verified target')
    qt = target_configuration(api, sha, version)
    gates = wait_for_gates(api, sha, qt, args.wait_seconds)
    artifacts = select_artifacts(api, gates, sha, version)
    if args.operation == 'verify':
        automation_sha = commit_id(subprocess.check_output(
            ['git', 'rev-parse', 'HEAD'], text=True).strip())
        if os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
                output.write(f'target_sha={sha}\nautomation_sha={automation_sha}\n')
        print(f'All six required CI jobs passed for {sha}.', flush=True)
        return
    require(args.expected_sha is not None, 'Publishing requires the read-only verification job target SHA')
    with tempfile.TemporaryDirectory(prefix='mergedesk-release-') as temporary:
        directory = Path(temporary)
        assets = validate_payloads(api, artifacts, directory, version, sha, qt)
        require(len(assets) == 5, 'Expected four packages and one SHA256SUMS manifest')
        publish_release(api, args.tag, sha, qt, gates, assets, directory)


if __name__ == '__main__':
    try:
        main()
    except (PromotionError, OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile, tarfile.TarError) as error:
        # Never print HTTP response bodies, signed download URLs, or credentials.
        print(f'Release promotion stopped: {error}', flush=True)
        raise SystemExit(1)
