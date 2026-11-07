# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [switch]$Portable,
    [switch]$Web
)
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path $Executable).Path
$taskTemporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('MergeDesk-tests-' + [guid]::NewGuid())
New-Item -ItemType Directory -Force $taskTemporaryDirectory | Out-Null
$encoding = [Text.UTF8Encoding]::new($false)
$previousPath = $env:PATH
$previousQpa = $env:QT_QPA_PLATFORM
$previousPluginPath = $env:QT_PLUGIN_PATH
$previousPlatformPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$previousQmlImportPath = $env:QML_IMPORT_PATH
$previousQml2ImportPath = $env:QML2_IMPORT_PATH
$previousWebEngineProcessPath = $env:QTWEBENGINEPROCESS_PATH
$previousWebEngineResourcesPath = $env:QTWEBENGINE_RESOURCES_PATH
$previousWebEngineLocalesPath = $env:QTWEBENGINE_LOCALES_PATH
$previousChromiumFlags = $env:QTWEBENGINE_CHROMIUM_FLAGS
$previousQuickBackend = $env:QT_QUICK_BACKEND
$results = [Collections.Generic.List[object]]::new()

function Write-Fixtures {
    $left = [Collections.Generic.List[string]]::new()
    $right = [Collections.Generic.List[string]]::new()
    for ($i = 1; $i -le 800; ++$i) {
        $left.Add("line $i common text here")
        if ($i % 100 -eq 0) { $right.Add("line $i CHANGED on right") }
        else { $right.Add("line $i common text here") }
        if ($i -eq 250) { $right.Add('extra right line') }
    }
    [IO.File]::WriteAllText((Join-Path $taskTemporaryDirectory 'left.txt'), ($left -join "`n")+"`n", $encoding)
    [IO.File]::WriteAllText((Join-Path $taskTemporaryDirectory 'right.txt'), ($right -join "`n")+"`n", $encoding)
    [IO.File]::WriteAllText((Join-Path $taskTemporaryDirectory 'left.csv'), "id,name,price`n1,apple,10`n2,banana,20`n3,cherry,30`n4,date,40`n", $encoding)
    [IO.File]::WriteAllText((Join-Path $taskTemporaryDirectory 'right.csv'), "id,name,price`n1,apple,11`n2,banana,20`n3,cereja,30`n5,elder,50`n", $encoding)
    foreach ($variant in @('A','B','C')) {
        [IO.File]::WriteAllText((Join-Path $taskTemporaryDirectory "three-$variant.txt"), "alpha`nversion = $variant`ncommon`n", $encoding)
    }
    $leftPng = 'iVBORw0KGgoAAAANSUhEUgAAAEAAAAAwCAYAAAChS3wfAAAAdElEQVR4nO3QQREAAAiAMPuX1hh7yBJwzD43OkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAO0AM+HSwnsOK34AAAAASUVORK5CYII='
    $rightPng = 'iVBORw0KGgoAAAANSUhEUgAAAEAAAAAwCAYAAAChS3wfAAAAiElEQVR4nO3QQQrDMBAEwX16fm6fc7INMkWibtBNLEPNsXmjB+gC0AN0AegBugD0AF0AeoDuPsDMs/cjBXD/ZwABBBBAAAEEEEAAmwL8aQHoAboA9ABdAHqALgA94Kr5zNdbfn/5xcUFEMDmAG8XgB6gC0AP0AWgB+gC0AN0AegBugD0AN32ACcCDIwDSDcRXgAAAABJRU5ErkJggg=='
    [IO.File]::WriteAllBytes((Join-Path $taskTemporaryDirectory 'left.png'), [Convert]::FromBase64String($leftPng))
    [IO.File]::WriteAllBytes((Join-Path $taskTemporaryDirectory 'right.png'), [Convert]::FromBase64String($rightPng))
}
function Run-Test([string]$Name, [string[]]$Files=@(), [string]$Language='') {
    Write-Fixtures
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.WorkingDirectory = $taskTemporaryDirectory
    if ($Language) { $start.Environment['MERGEDESK_LANGUAGE'] = $Language }
    $start.ArgumentList.Add('--'+$Name)
    foreach ($file in $Files) { $start.ArgumentList.Add((Join-Path $taskTemporaryDirectory $file)) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $clock = [Diagnostics.Stopwatch]::StartNew()
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $finished = $process.WaitForExit(180000)
    if (!$finished) { $process.Kill($true); $process.WaitForExit() }
    $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    $code = if ($finished) { $process.ExitCode } else { -1 }
    $record = [ordered]@{ test=$Name; language=$Language; passed=($code -eq 0); exit_code=$code; seconds=[Math]::Round($clock.Elapsed.TotalSeconds, 3); output=$output }
    $results.Add($record)
    $label = if ($Language) { "$Name [$Language]" } else { $Name }
    if ($code -ne 0) { Write-Host "FAIL: $label (exit $code)`n$output" }
    else { Write-Host "ok: $label" }
    $process.Dispose()
}
try {
    if ($Portable) {
        # The executable must resolve its own dependencies. Developer Qt/vcpkg
        # paths cannot accidentally make a broken portable package pass.
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:QT_PLUGIN_PATH = ''
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = ''
        $env:QML_IMPORT_PATH = ''
        $env:QML2_IMPORT_PATH = ''
        $env:QTWEBENGINEPROCESS_PATH = ''
        $env:QTWEBENGINE_RESOURCES_PATH = ''
        $env:QTWEBENGINE_LOCALES_PATH = ''
    }
    $env:QT_QPA_PLATFORM = 'offscreen'
    foreach ($test in @('selftest-merge','selftest-save','selftest-undo','selftest-nav','selftest-copy')) {
        Run-Test $test @('left.txt','right.txt')
    }
    foreach ($test in @('selftest-command-line','selftest-codepage','selftest-eol','selftest-table-edit','selftest-table-3way',
        'selftest-table-options','selftest-image-options','selftest-tree-sitter','selftest-subfolders',
        'selftest-folder3','selftest-folder-content','selftest-compare-methods','selftest-hex',
        'selftest-hex-edit','selftest-archive','selftest-archive-mixed','selftest-find-replace',
        'selftest-reload','selftest-filters','selftest-theme','selftest-safety','selftest-mergedesk-features')) {
        Run-Test $test
    }
    Run-Test 'selftest-table' @('left.csv','right.csv')
    Run-Test 'selftest-image' @('left.png','right.png')
    Run-Test 'selftest-merge3' @('three-A.txt','three-B.txt','three-C.txt')
    foreach ($language in @('en_US','hu','de','fr')) {
        foreach ($test in @('selftest-about','selftest-options','selftest-qt-i18n')) {
            Run-Test -Name $test -Language $language
        }
    }
    if ($Web) {
        $env:QT_QPA_PLATFORM = 'windows'
        # Hosted runners expose virtual GPUs. Keep the native Windows platform
        # and the same web assertions while using the Linux CI software setup.
        $env:QTWEBENGINE_CHROMIUM_FLAGS = "$previousChromiumFlags --disable-gpu".Trim()
        $env:QT_QUICK_BACKEND = 'software'
        Write-Host 'Web selftest: native Windows platform, software rendering'
        Run-Test 'selftest-web'
    }
    $report = [ordered]@{
        platform='windows-x64'; executable=[IO.Path]::GetFileName($Executable);
        portable_environment=[bool]$Portable; utc=[DateTime]::UtcNow.ToString('o'); tests=$results
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path (Split-Path $Executable) 'TEST-RESULTS.json')
    $failures = @($results | Where-Object { !$_.passed }).Count
    if ($failures) { throw "$failures application selftest(s) failed" }
} finally {
    $env:PATH = $previousPath
    $env:QT_QPA_PLATFORM = $previousQpa
    $env:QT_PLUGIN_PATH = $previousPluginPath
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $previousPlatformPath
    $env:QML_IMPORT_PATH = $previousQmlImportPath
    $env:QML2_IMPORT_PATH = $previousQml2ImportPath
    $env:QTWEBENGINEPROCESS_PATH = $previousWebEngineProcessPath
    $env:QTWEBENGINE_RESOURCES_PATH = $previousWebEngineResourcesPath
    $env:QTWEBENGINE_LOCALES_PATH = $previousWebEngineLocalesPath
    $env:QTWEBENGINE_CHROMIUM_FLAGS = $previousChromiumFlags
    $env:QT_QUICK_BACKEND = $previousQuickBackend
    Remove-Item -Recurse -Force $taskTemporaryDirectory
}
