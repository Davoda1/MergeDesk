# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')][string]$ExpectedVersion = '1.0.0.0'
)
$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$manifestTool = (Get-Command mt.exe -CommandType Application -ErrorAction Stop).Source
$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('MergeDesk-manifest-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
try {
    # Inspect RT_MANIFEST resource #1 in the actual executable, not its source XML.
    $manifestPath = Join-Path $temporaryDirectory 'embedded.manifest'
    & $manifestTool -nologo "-inputresource:$executablePath;#1" "-out:$manifestPath"
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract the embedded application manifest' }
    & $manifestTool -nologo -manifest $manifestPath -validate_manifest
    if ($LASTEXITCODE -ne 0) { throw 'The embedded application manifest is invalid' }

    $settings = [Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $reader = [Xml.XmlReader]::Create($manifestPath, $settings)
    $document = [Xml.XmlDocument]::new()
    $document.XmlResolver = $null
    try { $document.Load($reader) } finally { $reader.Dispose() }
    $namespaces = [Xml.XmlNamespaceManager]::new($document.NameTable)
    $namespaces.AddNamespace('asm', 'urn:schemas-microsoft-com:asm.v1')
    $namespaces.AddNamespace('asm3', 'urn:schemas-microsoft-com:asm.v3')
    $namespaces.AddNamespace('compat', 'urn:schemas-microsoft-com:compatibility.v1')
    $namespaces.AddNamespace('paths', 'http://schemas.microsoft.com/SMI/2016/WindowsSettings')
    $namespaces.AddNamespace('encoding', 'http://schemas.microsoft.com/SMI/2019/WindowsSettings')

    $identities = $document.SelectNodes('/asm:assembly/asm:assemblyIdentity', $namespaces)
    if ($identities.Count -ne 1 -or $identities[0].GetAttribute('name') -cne 'davoda1.MergeDesk' -or
        $identities[0].GetAttribute('version') -cne $ExpectedVersion) {
        throw 'The embedded application identity or version is incorrect'
    }
    $levels = $document.SelectNodes('/asm:assembly/asm3:trustInfo/asm3:security/asm3:requestedPrivileges/asm3:requestedExecutionLevel', $namespaces)
    if ($levels.Count -ne 1 -or $levels[0].GetAttribute('level') -cne 'asInvoker' -or
        $levels[0].GetAttribute('uiAccess') -cne 'false') {
        throw 'The application must run as the current user without elevated UI access'
    }
    $codePages = $document.SelectNodes('/asm:assembly/asm3:application/asm3:windowsSettings/encoding:activeCodePage', $namespaces)
    if ($codePages.Count -ne 1 -or $codePages[0].InnerText.Trim() -cne 'UTF-8') {
        throw 'The embedded manifest must enable the UTF-8 process code page'
    }
    $longPaths = $document.SelectNodes('/asm:assembly/asm3:application/asm3:windowsSettings/paths:longPathAware', $namespaces)
    if ($longPaths.Count -ne 1 -or $longPaths[0].InnerText.Trim() -cne 'true') {
        throw 'The embedded manifest must enable long Windows paths'
    }
    $supported = $document.SelectNodes('/asm:assembly/compat:compatibility/compat:application/compat:supportedOS', $namespaces)
    if (@($supported | Where-Object { $_.GetAttribute('Id') -ieq '{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}' }).Count -ne 1) {
        throw 'The embedded manifest must declare Windows 10 and Windows 11 compatibility'
    }
    Write-Host "Verified embedded manifest: davoda1.MergeDesk $ExpectedVersion, UTF-8, long paths, asInvoker"
} finally {
    Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
}
