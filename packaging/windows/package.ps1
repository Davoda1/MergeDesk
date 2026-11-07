# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
[CmdletBinding()]
param(
    [string]$BuildDirectory='build',
    [string]$OutputDirectory='dist'
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = (Resolve-Path $BuildDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$packageName = 'MergeDesk-1.0.0-windows-x64'
$package = Join-Path $output $packageName
$exe = Join-Path $build 'app/MergeDesk.exe'
if (!(Test-Path $exe)) { throw "Native application missing: $exe" }
$prefix = [IO.File]::ReadAllBytes($exe)[0..1]
if ($prefix[0] -ne 0x4d -or $prefix[1] -ne 0x5a) { throw 'Application is not a PE executable' }
if (Test-Path $package) { Remove-Item -Recurse -Force $package }
New-Item -ItemType Directory -Force $package | Out-Null
Copy-Item $exe $package
Get-ChildItem (Join-Path $build 'app') -Filter '*.dll' | Copy-Item -Destination $package
foreach ($directory in @('Filters','TreeSitterGrammars')) {
    $path = Join-Path (Join-Path $build 'app') $directory
    if (!(Test-Path $path)) { throw "Required application data missing: $path" }
    Copy-Item -Recurse $path $package
}
$qtRoot = $env:QT_ROOT_DIR
if (!$qtRoot) { $qtRoot = Split-Path (Split-Path (Get-Command windeployqt.exe).Source) }
$qtVersion = (& (Join-Path $qtRoot 'bin/qmake.exe') -query QT_VERSION).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot query the selected Qt SDK version' }
$qtRelease = Get-Content -Raw (Join-Path $source 'packaging/qt-runtime.json') | ConvertFrom-Json
if ($qtVersion -ne $qtRelease.version) {
    throw "Release packaging requires the reviewed Qt $($qtRelease.version) runtime; found $qtVersion. Update the runtime pin, notices, source records, and native validation together."
}
# A stale build must not acquire an unrelated runtime merely because it is
# first in PATH. CMake records the SDK selected for each linked Qt module.
$qtPrefix = [IO.Path]::GetFullPath($qtRoot).Replace('/', '\').TrimEnd('\') + '\'
$cmakeCache = Get-Content -Raw (Join-Path $build 'CMakeCache.txt')
foreach ($module in @('Qt6', 'Qt6Core', 'Qt6WebEngineCore', 'Qt6Svg')) {
    $match = [regex]::Match($cmakeCache, "(?m)^${module}_DIR:PATH=([^\r\n]+)")
    if (!$match.Success) { throw "CMake did not record the selected $module SDK" }
    $moduleDirectory = [IO.Path]::GetFullPath($match.Groups[1].Value).Replace('/', '\')
    if (!$moduleDirectory.StartsWith($qtPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "CMake selected $module from a different Qt SDK. Reconfigure the build before packaging."
    }
}
# The supported Windows 10/11 systems provide their own system D3D compiler.
# Qt's default D3D11 backend does not require the optional D3D12 shader compiler.
# Keep OS/SDK components out of the redistribution; package the application CRT below.
& (Join-Path $qtRoot 'bin/windeployqt.exe') --release --no-compiler-runtime --no-opengl-sw `
    --no-system-d3d-compiler --no-system-dxc-compiler `
    --include-plugins qoffscreen `
    --dir $package --plugindir (Join-Path $package 'plugins') (Join-Path $package 'MergeDesk.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }
# windeployqt merges module catalogs into qt_<language>.qm. MergeDesk loads
# qtbase_<language>.qm explicitly, so retain those original native Qt catalogs.
foreach ($language in @('hu','de','fr')) {
    $catalog = Join-Path $qtRoot "translations/qtbase_$language.qm"
    if (!(Test-Path $catalog)) { throw "Required Qt translation missing: $catalog" }
    Copy-Item $catalog (Join-Path $package 'translations')
}
$runtimeRoot = $env:VCToolsRedistDir
if (!$runtimeRoot) { throw 'VCToolsRedistDir must point to the MSVC redistributable runtime' }
$crt = Get-ChildItem (Join-Path $runtimeRoot 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (!$crt) { throw 'MSVC x64 CRT directory not found' }
Get-ChildItem $crt.FullName -Filter '*.dll' | Copy-Item -Destination $package
@'
[Paths]
Prefix=.
Plugins=plugins
Translations=translations
LibraryExecutables=.
Data=.
'@ | Set-Content -Encoding utf8 (Join-Path $package 'qt.conf')
foreach ($name in @('LICENSE','THIRD-PARTY-NOTICES.md','README.md','SECURITY.md')) { Copy-Item (Join-Path $source $name) $package }
if (Test-Path (Join-Path $source 'docs/MANUAL.md')) { Copy-Item (Join-Path $source 'docs/MANUAL.md') (Join-Path $package 'USER-MANUAL.md') }
Copy-Item -Recurse (Join-Path $source 'docs') $package
New-Item -ItemType Directory -Force (Join-Path $package '.github') | Out-Null
Copy-Item -Recurse (Join-Path $source '.github/workflows') (Join-Path $package '.github')
Copy-Item -Recurse (Join-Path $source 'packaging/integration') (Join-Path $package 'integration')
Copy-Item -Recurse (Join-Path $source 'licenses') $package
$licenseRoot = Join-Path $package 'licenses'
$vcpkgRoot = Join-Path $build 'vcpkg_installed/x64-windows'
$dependencyLicenses = Join-Path $licenseRoot 'vcpkg'
New-Item -ItemType Directory -Force $dependencyLicenses | Out-Null
Get-ChildItem (Join-Path $vcpkgRoot 'share') -Directory | ForEach-Object {
    $copyright = Join-Path $_.FullName 'copyright'
    if (Test-Path $copyright) { Copy-Item $copyright (Join-Path $dependencyLicenses ($_.Name+'.txt')) }
}
Copy-Item (Join-Path $build 'vcpkg_installed/vcpkg/status') (Join-Path $dependencyLicenses 'package-status.txt')
$noticeDirectory = Join-Path $licenseRoot "qt/$qtVersion"
if (!(Test-Path (Join-Path $noticeDirectory 'component-notices.html'))) {
    throw "Complete third-party notices are missing for Qt $qtVersion. Run packaging/collect_qt_licenses.py before packaging."
}
$noticeManifest = Get-Content -Raw (Join-Path $noticeDirectory 'component-notices-manifest.json') | ConvertFrom-Json
if ($noticeManifest.qt_version -ne $qtVersion) { throw 'Qt binary and license manifest version mismatch' }
$sourceArchives = @(Get-Content -Raw (Join-Path $noticeDirectory 'source-archives.json') | ConvertFrom-Json)
if ($sourceArchives.Count -eq 0) { throw 'Qt corresponding-source records are missing' }
foreach ($archive in $sourceArchives) {
    if ($archive.version -ne $qtVersion -or $archive.sha256 -notmatch '^[0-9a-f]{64}$' -or
        $archive.url -notmatch '^https://download\.qt\.io/(official_releases|archive)/qt/') {
        throw 'Qt corresponding-source record does not match the reviewed runtime'
    }
}
if (Test-Path (Join-Path $qtRoot 'sbom')) { Copy-Item -Recurse (Join-Path $qtRoot 'sbom') (Join-Path $licenseRoot 'qt/sdk-sbom') }
$revision = if ($env:GITHUB_ACTIONS -eq 'true' -and $env:GITHUB_SHA -match '^[0-9a-fA-F]{40}$') {
    $env:GITHUB_SHA.ToLowerInvariant()
} else {
    (& git -C $source rev-parse HEAD).Trim()
}
$dlls = @(Get-ChildItem $package -Recurse -File -Filter '*.dll' | ForEach-Object {
    [ordered]@{ path=[IO.Path]::GetRelativePath($package, $_.FullName).Replace('\','/'); version=$_.VersionInfo.FileVersion; sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[ordered]@{
    application='MergeDesk'; version='1.0.0'; platform='windows-x64'; configuration='Release';
    source_revision=$revision; qt=$qtVersion; compiler='MSVC 2022';
    vcpkg_baseline='351757dd7b0c6518412ca98b3ef7982861a47fc6';
    package_format='portable ZIP'; runtime_linkage='dynamic';
    minimum_os='Windows 10 1903 or newer / Windows 11'; utc=[DateTime]::UtcNow.ToString('o'); libraries=$dlls
} | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $package 'BUILD-METADATA.json')
foreach ($required in @('QtWebEngineProcess.exe','Qt6WebEngineCore.dll','resources/qtwebengine_resources.pak','plugins/platforms/qwindows.dll','plugins/platforms/qoffscreen.dll','translations/qtwebengine_locales/en-US.pak')) {
    if (!(Test-Path (Join-Path $package $required))) { throw "Portable runtime is incomplete: $required" }
}
& (Join-Path $PSScriptRoot 'verify_manifest.ps1') -Executable (Join-Path $package 'MergeDesk.exe')
& (Join-Path $PSScriptRoot 'run-selftests.ps1') -Executable (Join-Path $package 'MergeDesk.exe') -Portable -Web
$buildTreeReport = Join-Path (Split-Path $exe) 'TEST-RESULTS.json'
if (Test-Path $buildTreeReport) {
    Copy-Item $buildTreeReport (Join-Path $package 'BUILD-TREE-TEST-RESULTS.json')
}
$zip = Join-Path $output ($packageName+'.zip')
if (Test-Path $zip) { Remove-Item $zip }
[IO.Compression.ZipFile]::CreateFromDirectory($package, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($zip))" | Set-Content -Encoding ascii ($zip+'.sha256')
Write-Host "Created portable package: $zip"
