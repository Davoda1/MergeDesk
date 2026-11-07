# Validating MergeDesk 1.0.0

This guide describes reproducible checks, the behavior they cover and their
limits. Results belong to the exact source revision, executable and runtime
used for a run. This document does not assert that an untested checkout or
package has passed. See [BUILDING.md](BUILDING.md) for dependencies and build
commands, [COMPATIBILITY.md](COMPATIBILITY.md) for supported behavior, and
[PROVENANCE.md](PROVENANCE.md) for the source baseline.

## Repository analysis

The [repository checks workflow](../.github/workflows/checks.yml) validates
GitHub Actions definitions and scans the checked-out files for recognizable
secrets. Its tools are version-pinned and verified against their archive
checksums. [Repository check instructions](../packaging/checks/README.md)
provide the exact commands and explain the scan's scope. These checks
complement compilation and runtime tests; their results apply to the files and
rules used in that run.

## Source-build checks

The [native build workflow](../.github/workflows/build.yml) compiles Release
builds with MSVC on Windows and GCC on Ubuntu and Debian. CTest exercises the
engine, filters, encodings, syntax parsers and native image comparison code:

```sh
ctest --test-dir build --output-on-failure --parallel 3
```

On GNU/Linux, run the application regression runner and the focused safety,
feature and webpage suites against the built executable:

```sh
packaging/run_selftests.sh "$PWD/build/app/mergedesk"
QT_QPA_PLATFORM=offscreen build/app/mergedesk --selftest-safety
QT_QPA_PLATFORM=offscreen build/app/mergedesk --selftest-mergedesk-features
QT_QPA_PLATFORM=xcb QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
  QT_QUICK_BACKEND=software xvfb-run -a build/app/mergedesk --selftest-web
```

The regression runner creates fresh fixtures and isolates the working
directory, settings and cache. On Windows, run the PowerShell runner from a
Developer PowerShell with the Qt runtime on PATH:

```powershell
./packaging/windows/run-selftests.ps1 -Executable build/app/MergeDesk.exe -Web
```

The Windows runner writes `TEST-RESULTS.json` beside the executable. A test
group can contain many assertions; process counts, CTest cases and assertion
counts are different measures. Available optional helpers and platform-specific
checks can also change the enabled counts. Retain the actual report instead of
substituting totals from another build.

## Coverage

| Area | Checks and expected behavior |
| --- | --- |
| Text comparison and merging | Two- and three-way input, aligned and moved blocks, navigation, search, difference copying, whole-buffer automatic merges, undo and redo. |
| File safety | Exact saved bytes for mixed CRLF/LF/CR, missing final newlines, BOMs and legacy encodings; failed conversions leave destinations unchanged; backups retain earlier data. |
| Output encoding | UTF-8/UTF-16 BOMs and surrogate pairs, legacy Windows-1252 recovery, undo/redo, ghost alignment and final-line preservation in text and CSV views. |
| Read-only input | Disabled editing actions and direct command paths, including table editing, copying, saving and pane swaps. |
| Other comparison views | Folder operations, table editing, image options, hex editing, archives and comparison-method selection. |
| Projects and interchange | Project/settings round trips, converter executable selections protected from import, exported reports and exact patch reconstruction against deterministic fixtures. |
| Document conversion | Built-in XML package extraction, malformed inputs and resource limits; optional external converter behavior when the helper is available. |
| PDF and legacy Office | Real optional helper execution must be validated separately from substitute-helper invocation and failure checks. |
| Webpages | Real local pages in Qt WebEngine, rendered text and DOM, viewport/full-page pixels, synchronized scrolling and restored geometry after capture; relative resources remain usable while outside-folder file reads and outbound URL requests are rejected, including worker requests. |
| User interface | Themes, navigation, About and Options, and Qt widget translations in English, Hungarian, German and French. |
| Desktop integration | Generated commands and descriptors, installation/removal ownership, executable-path quoting, malformed configuration and symlink guards. |

GUI selftests use actual Qt widgets. Headless widget checks usually select
Qt's offscreen platform; webpage checks use a virtual X11 display on GNU/Linux
and the native Windows platform plugin on Windows. Software rendering avoids
depending on a CI runner's virtual GPU. These test-process settings do not
change normal application rendering defaults.

Ordinary application launches keep Chromium's sandbox enabled. The Debian
workflow uses a test-only sandbox override in its isolated root-run container.
Run desktop validation as a regular user; the container override is not a
recommended desktop configuration.

## Native file-dialog checks

Run the GNU/Linux native-dialog fixture against the actual executable:

```sh
/usr/bin/python3 packaging/linux/verify_native_dialogs.py \
  --executable build/app/mergedesk --output build/NATIVE-DIALOGS.json
```

The fixture starts an isolated D-Bus session and uses an existing X display or
Xvfb. It services real Qt FileChooser requests while identifying the desktop
as KDE and GNOME in separate processes. Open-file, save-file and choose-folder
operations must return the exact requested fixture paths. The report records
request options, response URIs, Qt's selected paths and the loaded portal
plugin. Additional checks cover explicit empty/nonempty platform-theme
settings, Qt's command-line theme override and offscreen/minimal selections.

For a relocated distribution, add
`--expected-plugin-root /absolute/package/plugins`. For an installed DEB, use
`--expected-plugin-root /usr/lib/mergedesk/plugins`. These checks require the
portal plugin to come from the packaged runtime. The fixture does not replace
the desktop's own portal service.

The host desktop needs `xdg-desktop-portal` and the appropriate KDE/GNOME
FileChooser backend for normal portal-native dialogs. Automated request routing
does not verify every backend's visual appearance, Wayland compositor or
interactive desktop behavior.

## Package checks

Build packages using [BUILDING.md](BUILDING.md). Packaging launches relocated
copies with development Qt and library paths removed so an installed SDK cannot
hide a missing dependency. Keep the generated `BUILD-METADATA.json` and
`TEST-RESULTS.json` with the corresponding artifact.

| Distribution | Validation scope |
| --- | --- |
| Windows portable ZIP | Run the real `MergeDesk.exe` at the archive root; exercise application and web suites with development Qt/vcpkg paths removed from PATH. |
| GNU/Linux portable TAR.GZ | Run the real root-level `mergedesk` ELF executable with the bundled runtime, then run the package's GUI and native-dialog checks. |
| AppImage | Exercise the built AppImage and extracted `AppRun` path as defined in the native build workflow; verify the packaged runtime rather than a source-tree executable. |
| Debian package | Install the generated DEB, run the real `/usr/bin/mergedesk` binary and check the installed private runtime and portal plugin. |

Each platform needs its own completed run. Linux results do not establish
Windows behavior, and testing a source-tree executable does not establish that
an archive remains functional after extraction on another machine. A package
report's enabled checks and runtime versions determine the scope of its result.

## Optional converters

PDF text checks need `pdftotext`. Legacy DOC/XLS/PPT/RTF conversion needs an
installed LibreOffice executable. These tools are not bundled with MergeDesk.
Tests that use a substitute helper verify invocation, timeouts and failure
handling; they do not establish the behavior of a real converter.

When validating an optional helper, record its actual version and exercise
representative files through the application. Confirm that input documents
remain unchanged, output contains the expected text, temporary profiles and
outputs are removed, and failures reach the user. Repeat this on each platform
where that helper is intended to be supported.

## Performance smoke checks

The benchmark helper creates deterministic text, recursive-folder, CSV and PNG
fixtures, verifies the comparison result and checks that input data and metadata
remain unchanged. It records the executable hash, input manifests, elapsed time
and available child-process memory measurements:

```sh
python3 packaging/benchmark.py --executable /absolute/path/to/mergedesk \
  --output build/benchmark-results.json --brief build/benchmark-brief.md
```

Elapsed time includes process startup, comparison and shutdown; fixture
generation and verification are excluded. Each workload is one smoke sample.
Caches, CPU scheduling and concurrent workloads are not controlled. On Linux,
peak RSS is the individual child's high-water mark, not the sum of every
WebEngine or helper process. These measurements support regression investigation
and do not establish a general performance guarantee.

## Interactive review

Review the application on Windows and on KDE/GNOME desktops with both light and
dark themes. Check native open/save/folder dialogs, window decorations, keyboard
navigation, high-DPI scaling and readable layouts in all supported languages.
Exercise the installed actions in Explorer, Dolphin, Nautilus, Nemo and Thunar
where those integrations are used. Automated descriptor tests do not certify
every version of those file managers.

The [application screenshots](screenshots/README.md) show actual Qt captures.
They illustrate appearance and do not replace package execution, accessibility
review or regression results. Automated checks remain scoped to their fixtures;
the [compatibility matrix](COMPATIBILITY.md) documents differences from WinMerge.
