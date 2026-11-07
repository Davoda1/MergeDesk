#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Build self-contained native Linux tar, DEB and AppImage distributions.

Build release binaries in Debian 12 to keep the glibc baseline at 2.36. The
portable archive contains a real ELF executable at its root. The DEB installs
that executable as /usr/bin/mergedesk; Qt stays in /usr/lib/mergedesk.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import tarfile
import tempfile
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
VERSION = '1.0.0'
SYSTEM_LIBRARIES = re.compile(r'^(?:ld-linux.*|libc\.so\..*|libm\.so\..*|libmvec\.so\..*|libpthread\.so\..*|libdl\.so\..*|librt\.so\..*|libresolv\.so\..*|libutil\.so\..*|libanl\.so\..*)$')


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, text=True, **kwargs)


def capture(command):
    return subprocess.check_output([str(x) for x in command], text=True).strip()


def source_revision():
    # The CI checkout commit is supplied by GitHub. Using that value avoids
    # changing Git's ownership protections for mounted container workspaces.
    revision = os.environ.get('GITHUB_SHA', '')
    if os.environ.get('GITHUB_ACTIONS') == 'true' and re.fullmatch(r'[0-9a-fA-F]{40}', revision):
        return revision.lower()
    return capture(['git','-C',ROOT,'rev-parse','HEAD'])


def copy_tree(source: Path, destination: Path):
    if not source.is_dir():
        raise RuntimeError(f'Required data directory missing: {source}')
    shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=False,
                    ignore=lambda directory, names: [name for name in names
                        if is_copy_temporary(Path(directory)/name)])


def is_elf(path: Path):
    try:
        with path.open('rb') as stream:
            return stream.read(4) == b'\x7fELF'
    except OSError:
        return False


def is_copy_temporary(path: Path):
    match = re.fullmatch(r'\.(.+)\.[A-Za-z0-9]{6}', path.name)
    return bool(match and is_elf(path.with_name(match.group(1))))


def discard_copy_temporaries(directory: Path):
    # Some copy/ELF rewrite backends leave their temporary siblings visible.
    # Only discard a recognizable temporary whose complete published ELF
    # already exists. Never include half-written temporary objects in a bundle.
    for path in list(directory.rglob('.*')):
        if path.is_file() and is_copy_temporary(path):
            path.unlink(missing_ok=True)


def dependencies(path: Path, qt_libraries: Path | None = None):
    # The first pass resolves the official SDK's relocatable plugins before
    # their dependencies have been copied. The final pass must not inherit a
    # developer's LD_LIBRARY_PATH and accidentally validate an incomplete bundle.
    environment = os.environ.copy()
    environment['LD_LIBRARY_PATH'] = str(qt_libraries) if qt_libraries else ''
    text = subprocess.check_output(['ldd', str(path)], text=True, env=environment).strip()
    if 'not found' in text:
        raise RuntimeError(f'Unresolved dependency of {path}:\n{text}')
    for line in text.splitlines():
        match = re.search(r'=>\s+(/\S+)', line)
        if not match:
            match = re.match(r'\s*(/\S+)\s+\(', line)
        if match:
            yield Path(match.group(1))


def fetch_tool(name: str, metadata: dict, cache: Path):
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / name
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == metadata['sha256']:
        return path
    data = urllib.request.urlopen(metadata['url'], timeout=120).read()
    if hashlib.sha256(data).hexdigest() != metadata['sha256']:
        raise RuntimeError(f'{name} checksum mismatch; update pinned metadata only after reviewing the new release')
    path.write_bytes(data)
    path.chmod(0o755)
    return path


def source_archives(name: str, version: str, required: bool):
    """Resolve corresponding source archives from APT's signed source index."""
    index = subprocess.run(['apt-cache', 'showsrc', '--only-source', name],
                           text=True, capture_output=True)
    fields = {}
    for stanza in index.stdout.split('\n\n'):
        candidate = {}
        field = None
        for line in stanza.splitlines():
            if line.startswith(' ') and field:
                candidate[field] += '\n'+line.strip()
            elif ': ' in line:
                field, value = line.split(': ', 1)
                candidate[field] = value
            elif line.endswith(':'):
                field = line[:-1]
                candidate[field] = ''
        if candidate.get('Package') == name and candidate.get('Version') == version:
            fields = candidate
            break
    requested = subprocess.run(['apt-get', '--print-uris', 'source', '--download-only',
                                '--only-source', name+'='+version],
                               text=True, capture_output=True)
    checksums = {}
    for line in fields.get('Checksums-Sha256', '').splitlines():
        items = line.split()
        if len(items) == 3:
            checksums[items[2]] = (items[0], int(items[1]))
    result = []
    for line in requested.stdout.splitlines():
        if not line.startswith("'"):
            continue
        items = shlex.split(line)
        filename = Path(urllib.parse.unquote(urllib.parse.urlsplit(items[0]).path)).name
        if filename not in checksums:
            raise RuntimeError(f'APT source URI lacks signed SHA-256 metadata: {filename}')
        digest, size = checksums[filename]
        result.append(dict(url=items[0], filename=filename, sha256=digest, size=size))
    if required and not result:
        raise RuntimeError(f'Corresponding source metadata unavailable for {name}={version}. Enable deb-src and update APT indexes before packaging.')
    return result


def release_qt_metadata():
    metadata = json.loads((ROOT/'packaging/qt-runtime.json').read_text())
    version = metadata['version']
    notices = (ROOT/metadata['notices_directory']).resolve()
    if notices.name != version or not notices.is_relative_to((ROOT/'licenses/qt').resolve()):
        raise RuntimeError('The Qt notice directory must be inside licenses/qt')
    manifest = json.loads((notices/'component-notices-manifest.json').read_text())
    if manifest.get('qt_version') != version or not (notices/'component-notices.html').is_file():
        raise RuntimeError(f'Complete component notices are missing for Qt {version}')
    archives = json.loads((notices/'source-archives.json').read_text())
    if not archives or any(record.get('version') != version or not valid_source_archive(record)
                           or not re.match(r'^https://download\.qt\.io/(?:official_releases|archive)/qt/',
                                           record.get('url', ''))
                           for record in archives):
        raise RuntimeError(f'Corresponding source metadata is incomplete for Qt {version}')
    return metadata, notices


def valid_source_archive(record: dict):
    return (record.get('url', '').startswith('https://')
            and re.fullmatch(r'[0-9a-f]{64}', record.get('sha256', '')) is not None)


def cmake_cache_values(build: Path):
    values = {}
    for line in (build/'CMakeCache.txt').read_text().splitlines():
        match = re.fullmatch(r'([^/#][^:]*):[^=]+=(.*)', line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def qt_sdk_configuration(qt_root: Path, build: Path):
    """Select the reviewed SDK and reject an accidentally mixed Qt build."""
    qt_root = qt_root.resolve(strict=True)
    metadata, notices = release_qt_metadata()
    qmake = next((qt_root/'bin'/name for name in ['qmake', 'qmake6']
                  if (qt_root/'bin'/name).is_file()), None)
    if qmake is None or not qmake.resolve().is_relative_to(qt_root):
        raise RuntimeError(f'The selected Qt SDK has no native qmake: {qt_root}')
    qt = dict(line.split(':', 1) for line in capture([qmake, '-query']).splitlines()
              if ':' in line)
    if qt.get('QT_VERSION') != metadata['version']:
        raise RuntimeError(f'Release packages require reviewed Qt {metadata["version"]}; '
                           f'the selected SDK reports {qt.get("QT_VERSION", "unknown")}')
    for key in ['QT_INSTALL_PREFIX', 'QT_INSTALL_LIBS', 'QT_INSTALL_PLUGINS',
                'QT_INSTALL_TRANSLATIONS', 'QT_INSTALL_DATA', 'QT_INSTALL_LIBEXECS']:
        path = Path(qt.get(key, '')).resolve()
        if not qt.get(key) or not path.is_dir() or not path.is_relative_to(qt_root):
            raise RuntimeError(f'{key} does not belong to the selected Qt SDK')
    if Path(qt['QT_INSTALL_PREFIX']).resolve() != qt_root:
        raise RuntimeError('qmake reports a different Qt SDK prefix')
    cache = cmake_cache_values(build)
    for key in ['Qt6_DIR', 'Qt6Core_DIR', 'Qt6WebEngineCore_DIR', 'Qt6Svg_DIR']:
        if key not in cache or not Path(cache[key]).resolve().is_relative_to(qt_root):
            raise RuntimeError(f'The application was not configured with the selected Qt SDK: {key}')
    return qt, notices


def sdk_library_component(source: Path, qt_root: Path, qt_version: str,
                          dependencies_metadata: dict):
    """Identify reviewed SDK libraries, never silently skip unknown ownership."""
    resolved = source.resolve(strict=True)
    if not resolved.is_relative_to(qt_root):
        if re.fullmatch(r'libQt6\w+\.so(?:\.[0-9]+)+', source.name):
            raise RuntimeError(f'A system Qt library would be mixed into the SDK runtime: {source}')
        return None
    if re.fullmatch(r'libQt6\w+\.so\.' + re.escape(qt_version), resolved.name):
        return 'qt'
    icu = dependencies_metadata.get('icu', {})
    if (icu and re.fullmatch(r'libicu(?:uc|i18n|data|io|tu|test)\.so\.' + re.escape(icu.get('version', '')),
                            resolved.name)):
        return 'icu'
    raise RuntimeError(f'Unreviewed library in the Qt SDK has no license/source mapping: {source}')


def collect_sdk_licenses(sources: set[Path], package: Path, qt_root: Path,
                         qt_version: str, notices: Path):
    dependencies_file = notices/'sdk-dependencies.json'
    dependencies_metadata = json.loads(dependencies_file.read_text()) if dependencies_file.is_file() else {}
    sdk_records = []
    system_sources = set()
    components = set()
    for source in sorted(sources):
        component = sdk_library_component(source, qt_root, qt_version, dependencies_metadata)
        if component is None:
            system_sources.add(source)
            continue
        components.add(component)
        sdk_records.append(dict(path=str(source.resolve().relative_to(qt_root)),
                                soname=source.name, component=component))
    for component in components - {'qt'}:
        metadata = dependencies_metadata[component]
        license_file = (notices/metadata.get('license_file', '')).resolve()
        if (not license_file.is_relative_to(notices.resolve()) or not license_file.is_file()
                or metadata.get('source', {}).get('version') != metadata.get('version')
                or not valid_source_archive(metadata.get('source', {}))):
            raise RuntimeError(f'License or corresponding source metadata is missing for SDK {component}')
    destination = package/'licenses/qt'/notices.name
    destination.mkdir(parents=True, exist_ok=True)
    # The committed exact-version notice inventory was copied with licenses/.
    # Preserve the SDK's additional SPDX inventory when one is supplied.
    if (qt_root/'sbom').is_dir():
        copy_tree(qt_root/'sbom', destination/'sdk-sbom')
    (destination/'deployed-sdk-libraries.json').write_text(json.dumps(sdk_records, indent=2)+'\n')
    return system_sources, sdk_records


def collect_system_licenses(sources: set[Path], package: Path, require_source_metadata: bool):
    license_directory = package / 'licenses/system-packages'
    license_directory.mkdir(parents=True, exist_ok=True)
    packages: set[str] = set()
    for source in sorted(sources):
        found = None
        candidates = [source, source.resolve()]
        if str(source).startswith('/lib/'):
            candidates.append(Path('/usr'+str(source)))
        for candidate in candidates:
            result = subprocess.run(['dpkg-query','-S',str(candidate)],text=True,capture_output=True)
            if result.returncode == 0:
                found = result.stdout.split(': ',1)[0].splitlines()[0]
                break
        if not found:
            raise RuntimeError(f'Cannot determine license/package ownership for bundled library {source}')
        packages.add(found)
    records = []
    source_records = {}
    for name in sorted(packages):
        fields = capture(['dpkg-query','-W','-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n',name]).split('\t')
        base = name.split(':')[0]
        copyright_file = Path('/usr/share/doc')/base/'copyright'
        if not copyright_file.exists():
            raise RuntimeError(f'License notices missing for {name}')
        shutil.copy2(copyright_file,license_directory/(base+'.copyright'))
        record = dict(zip(['package','version','source_package','source_version'],fields))
        key = record['source_package']+'='+record['source_version']
        if key not in source_records:
            source_records[key] = dict(package=record['source_package'], version=record['source_version'],
                archives=source_archives(record['source_package'], record['source_version'], require_source_metadata))
        records.append(record)
    copy_tree(Path('/usr/share/common-licenses'),license_directory/'common-licenses')
    (license_directory/'metadata.json').write_text(json.dumps(records,indent=2)+'\n')
    (license_directory/'corresponding-source.json').write_text(json.dumps(list(source_records.values()),indent=2)+'\n')
    return records


def make_deb(package: Path, output: Path):
    stage = output / '.deb-stage'
    if stage.exists(): shutil.rmtree(stage)
    binary = stage/'usr/bin/mergedesk'
    binary.parent.mkdir(parents=True)
    shutil.copy2(package/'mergedesk',binary)
    runtime = stage/'usr/lib/mergedesk'
    runtime.mkdir(parents=True)
    for name in ['lib','plugins','libexec','resources','translations','TreeSitterGrammars']:
        copy_tree(package/name,runtime/name)
    data = stage/'usr/share/mergedesk'
    data.mkdir(parents=True)
    copy_tree(package/'Filters',data/'Filters')
    if (ROOT/'packaging/integration').exists():
        copy_tree(ROOT/'packaging/integration',data/'integration')
    document = stage/'usr/share/doc/mergedesk'
    document.mkdir(parents=True)
    for name in ['LICENSE','THIRD-PARTY-NOTICES.md','README.md','SECURITY.md','BUILD-METADATA.json']:
        shutil.copy2(package/name,document/name)
    if (package/'USER-MANUAL.md').exists(): shutil.copy2(package/'USER-MANUAL.md',document/'USER-MANUAL.md')
    copy_tree(package/'docs',document/'docs')
    copy_tree(package/'.github/workflows',document/'.github/workflows')
    copy_tree(package/'licenses',document/'licenses')
    applications = stage/'usr/share/applications'
    applications.mkdir(parents=True)
    shutil.copy2(ROOT/'packaging/linux/mergedesk.desktop',applications/'mergedesk.desktop')
    metadata = stage/'usr/share/metainfo'
    metadata.mkdir(parents=True)
    shutil.copy2(ROOT/'packaging/linux/io.github.davoda1.mergedesk.appdata.xml',metadata/'io.github.davoda1.mergedesk.appdata.xml')
    for size in [16,32,64,128,256,512]:
        icon = stage/f'usr/share/icons/hicolor/{size}x{size}/apps/mergedesk.png'
        icon.parent.mkdir(parents=True)
        shutil.copy2(ROOT/f'packaging/linux/icons/mergedesk-{size}.png',icon)
    run(['patchelf','--set-rpath','$ORIGIN/../lib/mergedesk/lib',binary])
    control = stage/'DEBIAN'
    control.mkdir()
    glibc_minimum = json.loads((package/'BUILD-METADATA.json').read_text())['glibc_baseline'].split()[-1]
    size = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())//1024
    (control/'control').write_text(f'''Package: mergedesk
Version: {VERSION}
Architecture: amd64
Maintainer: davoda1
Section: devel
Priority: optional
Installed-Size: {size}
Depends: libc6 (>= {glibc_minimum}), ca-certificates, fonts-dejavu-core
Recommends: xdg-utils, xdg-desktop-portal
Homepage: https://github.com/Davoda1/MergeDesk
Description: Native Qt6 file, folder and visual comparison and merging
 Text and three-way merges, folders, images, binary data, tables,
 archives and rendered webpages with native light and dark themes.
 Qt6 and application libraries are bundled in a private runtime directory.
''')
    lines = []
    for path in sorted(stage.rglob('*')):
        if path.is_file() and control not in path.parents:
            lines.append(hashlib.md5(path.read_bytes()).hexdigest()+'  '+str(path.relative_to(stage)))
    (control/'md5sums').write_text('\n'.join(lines)+'\n')
    destination = output/f'mergedesk_{VERSION}_amd64.deb'
    run(['dpkg-deb','--root-owner-group','--build',stage,destination])
    if binary.is_symlink() or not is_elf(binary):
        raise RuntimeError('DEB must install a real ELF at /usr/bin/mergedesk')
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--qt-root',type=Path,required=True,
                        help='The reviewed official Qt SDK used to configure the application')
    parser.add_argument('--output-dir',type=Path,default=Path('dist'))
    parser.add_argument('--tool-cache',type=Path,default=Path('build-tools'))
    parser.add_argument('--require-debian12',action='store_true')
    parser.add_argument('--offscreen-web-test',action='store_true',
                        help='Use Qt software offscreen rendering where an X server cannot run')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True,exist_ok=True)
    glibc = capture(['getconf','GNU_LIBC_VERSION'])
    if args.require_debian12 and glibc != 'glibc 2.36':
        raise RuntimeError(f'Distribution builds require the Debian 12 glibc 2.36 baseline, got {glibc}')
    executable = build/'app/mergedesk'
    if not is_elf(executable): raise RuntimeError(f'Native ELF application missing: {executable}')
    qt_root = args.qt_root.resolve(strict=True)
    qt, qt_notices = qt_sdk_configuration(qt_root, build)
    qt_libraries = Path(qt['QT_INSTALL_LIBS'])
    name = f'MergeDesk-{VERSION}-linux-x86_64'
    package = output/name
    if package.exists(): shutil.rmtree(package)
    package.mkdir()
    shutil.copy2(executable,package/'mergedesk')
    print('Collecting reviewed Qt runtime and application data', flush=True)
    plugins = Path(qt['QT_INSTALL_PLUGINS'])
    for directory in ['platforms','imageformats','iconengines','styles','platformthemes','platforminputcontexts',
                      'xcbglintegrations','wayland-decoration-client','wayland-graphics-integration-client',
                      'wayland-shell-integration','tls','networkinformation']:
        if (plugins/directory).is_dir(): copy_tree(plugins/directory,package/'plugins'/directory)
    for theme in ['libqxdgdesktopportal.so','libqgtk3.so']:
        if not (package/'plugins/platformthemes'/theme).exists():
            raise RuntimeError(f'The selected SDK is missing native desktop dialog plugin {theme}')
    translations = Path(qt['QT_INSTALL_TRANSLATIONS'])
    (package/'translations').mkdir()
    for file in translations.glob('*.qm'): shutil.copy2(file,package/'translations'/file.name)
    for language in ['hu','de','fr']:
        if not (package/f'translations/qtbase_{language}.qm').exists():
            raise RuntimeError(f'The selected SDK is missing the Qt {language} catalog')
    copy_tree(translations/'qtwebengine_locales',package/'translations/qtwebengine_locales')
    copy_tree(Path(qt['QT_INSTALL_DATA'])/'resources',package/'resources')
    (package/'libexec').mkdir()
    shutil.copy2(Path(qt['QT_INSTALL_LIBEXECS'])/'QtWebEngineProcess',package/'libexec/QtWebEngineProcess')
    copy_tree(build/'app/Filters',package/'Filters')
    copy_tree(build/'app/TreeSitterGrammars',package/'TreeSitterGrammars')
    library_directory = package/'lib'
    library_directory.mkdir()
    sources: set[Path] = set()
    queue = [p for p in package.rglob('*') if p.is_file() and not is_copy_temporary(p) and is_elf(p)]
    # OpenSSL and NSS load several providers at runtime without DT_NEEDED.
    ldconfig = capture(['/sbin/ldconfig','-p'])
    for soname in ['libssl.so.3','libcrypto.so.3','libnssckbi.so','libsoftokn3.so','libfreebl3.so','libfreeblpriv3.so']:
        match = re.search(r'^\s*'+re.escape(soname)+r'\s+.*=>\s+(\S+)',ldconfig,re.M)
        if match:
            source = Path(match.group(1)); destination=library_directory/source.name
            shutil.copy2(source,destination);sources.add(source);queue.append(destination)
    scanned: set[Path] = set()
    while queue:
        path = queue.pop()
        if path in scanned: continue
        scanned.add(path)
        for source in dependencies(path, qt_libraries):
            if SYSTEM_LIBRARIES.match(source.name): continue
            destination = library_directory/source.name
            if not destination.exists():
                shutil.copy2(source,destination)
                sources.add(source)
                queue.append(destination)
    discard_copy_temporaries(package)
    print('Setting relative runtime search paths', flush=True)
    for path in list(package.rglob('*')):
        if path.is_file() and not is_copy_temporary(path) and is_elf(path):
            relative = os.path.relpath(library_directory,path.parent)
            rpath = '$ORIGIN' if relative == '.' else '$ORIGIN/'+relative
            run(['patchelf','--set-rpath',rpath,path],stdout=subprocess.DEVNULL)
    discard_copy_temporaries(package)
    (package/'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=plugins\nLibraries=lib\nLibraryExecutables=libexec\nTranslations=translations\nData=.\n')
    for item in ['LICENSE','THIRD-PARTY-NOTICES.md','README.md','SECURITY.md']:
        shutil.copy2(ROOT/item,package/item)
    if (ROOT/'docs/MANUAL.md').exists(): shutil.copy2(ROOT/'docs/MANUAL.md',package/'USER-MANUAL.md')
    copy_tree(ROOT/'docs',package/'docs')
    # Keep the build instructions' relative workflow link usable offline.
    copy_tree(ROOT/'.github/workflows',package/'.github/workflows')
    copy_tree(ROOT/'licenses',package/'licenses')
    if (ROOT/'packaging/integration').exists(): copy_tree(ROOT/'packaging/integration',package/'integration')
    print('Collecting dependency licenses and corresponding-source metadata', flush=True)
    system_sources, sdk_records = collect_sdk_licenses(sources,package,qt_root,qt['QT_VERSION'],qt_notices)
    records = collect_system_licenses(system_sources,package,args.require_debian12)
    env = os.environ.copy();env['QT_QPA_PLATFORM']='offscreen'
    # Relocated archive and AppImage checks must resolve their own runtime,
    # including the WebEngine helper and data, without the developer SDK.
    for key in ['LD_LIBRARY_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH',
                'QML_IMPORT_PATH','QML2_IMPORT_PATH','QTWEBENGINEPROCESS_PATH',
                'QTWEBENGINE_RESOURCES_PATH','QTWEBENGINE_LOCALES_PATH']:
        env.pop(key,None)
    if os.geteuid() == 0: env['QTWEBENGINE_DISABLE_SANDBOX']='1'  # Test/build helper only.
    metadata = dict(application='MergeDesk',version=VERSION,platform='linux-x86_64',configuration='Release',
                    source_revision=source_revision(),qt=qt['QT_VERSION'],
                    glibc_baseline=glibc,compiler=capture(['c++','-dumpfullversion']),runtime_linkage='dynamic',
                    system_packages=records,qt_sdk_libraries=sdk_records)
    (package/'BUILD-METADATA.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print('Auditing every deployed ELF dependency', flush=True)
    # Every shared dependency must resolve inside the bundle except the system
    # C runtime. This also verifies plugin and WebEngine helper dependencies.
    for path in package.rglob('*'):
        if path.is_file() and not is_copy_temporary(path) and is_elf(path):
            for dependency in dependencies(path):
                if not SYSTEM_LIBRARIES.match(dependency.name) and not dependency.resolve().is_relative_to(package):
                    raise RuntimeError(f'Unbundled dependency: {path}: {dependency}')
    # Relocate and run the actual application, never a wrapper.
    verification = [ROOT/'packaging/linux/verify.py','--package',package]
    if args.offscreen_web_test: verification.append('--offscreen-web-test')
    run(verification,env=env)
    archive = output/(name+'.tar.gz')
    with tarfile.open(archive,'w:gz',compresslevel=9) as tar:
        for path in sorted(package.iterdir()):
            tar.add(path,arcname=path.name,
                    filter=lambda member: None if is_copy_temporary(package/member.name) else member)
    deb = make_deb(package,output)
    appdir = output/'.AppDir'
    if appdir.exists(): shutil.rmtree(appdir)
    copy_tree(package,appdir)
    (appdir/'AppRun').symlink_to('mergedesk')
    shutil.copy2(ROOT/'packaging/linux/mergedesk.desktop',appdir/'mergedesk.desktop')
    shutil.copy2(ROOT/'packaging/linux/icons/mergedesk-256.png',appdir/'mergedesk.png')
    (appdir/'.DirIcon').symlink_to('mergedesk.png')
    tools = json.loads((ROOT/'packaging/linux/appimage-tools.json').read_text())
    tool = fetch_tool('appimagetool',tools['appimagetool'],args.tool_cache.resolve())
    runtime = fetch_tool('runtime',tools['runtime'],args.tool_cache.resolve())
    appimage = output/(name+'.AppImage')
    image_environment = os.environ.copy();image_environment.update(ARCH='x86_64',APPIMAGE_EXTRACT_AND_RUN='1')
    run([tool,'--no-appstream','--runtime-file',runtime,'--mksquashfs-opt=-no-xattrs',
         '--mksquashfs-opt=-processors','--mksquashfs-opt=2',appdir,appimage],env=image_environment)
    appimage.chmod(0o755)
    with tempfile.TemporaryDirectory(prefix='mergedesk-appimage-test-') as temporary:
        smoke = env.copy()
        smoke.update(APPIMAGE_EXTRACT_AND_RUN='1',XDG_CONFIG_HOME=temporary+'/config',
                     XDG_CACHE_HOME=temporary+'/cache')
        run([appimage,'--selftest-safety'],env=smoke,timeout=180)
        smoke.update(QTWEBENGINE_CHROMIUM_FLAGS='--disable-gpu',QT_QUICK_BACKEND='software')
        if args.offscreen_web_test:
            smoke['QT_QPA_PLATFORM']='offscreen'
            web_command=[appimage,'--selftest-web']
        else:
            smoke['QT_QPA_PLATFORM']='xcb'
            web_command=['xvfb-run','-a',appimage,'--selftest-web']
        run(web_command,env=smoke,timeout=180)
    (output/'APPIMAGE-BUILD-TOOLS.json').write_text(json.dumps(tools,indent=2)+'\n')
    for artifact in [archive,deb,appimage]:
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        artifact.with_name(artifact.name+'.sha256').write_text(digest+'  '+artifact.name+'\n')
        print('Created',artifact)


if __name__ == '__main__': main()
