# MergeDesk 1.0.0 compatibility

MergeDesk uses a native Qt 6 Widgets interface over the portable WinMerge
comparison engines adopted from LibreMerge. The functional reference is
[WinMerge 2.16.60](https://github.com/WinMerge/winmerge/releases/tag/v2.16.60),
released 2026-10-27. [PROVENANCE.md](PROVENANCE.md) records the exact source
commits, later engine updates, and retained licenses.

This document distinguishes working native equivalents from Windows-specific
features that have no equivalent in this release. It does not claim complete
compatibility with every WinMerge plugin, command-line switch or UI behavior.

## Feature matrix

| Area | MergeDesk implementation | Compatibility boundary |
| --- | --- | --- |
| Text comparison | Two and three editable, aligned panes; line and word differences; moved blocks; minimap; navigation; copy/merge; undo/redo; search and replace | Uses the vendored WinMerge diff algorithms. Very large files still have the engine's memory and runtime costs. |
| Three-way merging | Manual copy in either direction and automatic merge with conflict markers | Automatic merge uses the chosen destination's current content as the ancestor. There is no separate fourth Merge Result pane matching the newer WinMerge MFC layout. |
| Comparison rules | Case, whitespace, blank lines, numbers, EOL, codepage, missing final EOL and comment handling; Myers/minimal/patience/histogram choices | Syntax-aware rules depend on the selected language and available parsers. |
| Folder comparison | Two or three roots; recursive or top-level scan; tree/flat displays; multiple comparison methods; file filters; copy/delete operations | Windows Shell property providers and their property-dependent filters are not a portable metadata API. |
| Table comparison | Editable CSV/TSV/delimited grids, quoted multiline fields, difference copying and column rules | Office spreadsheets are extracted to cell-address text, not opened as an editable Excel grid. |
| Image comparison | WinIMerge algorithm over QImage; block/pixel differences, overlays, blink, threshold, zoom, transforms and copying | Read/write formats depend on Qt image plugins. Reading multipage GIF/TIFF does not imply an equivalent multipage writer. There is no WinIMerge Windows PDF/EMF/WMF conversion backend. |
| Binary comparison | Hex display/editing, navigation, find/replace, copying and undo | Unified text patches cannot describe arbitrary binary changes. |
| Archives | libarchive extraction of ordinary files and directories into temporary directories; archive/folder comparisons | Symbolic links, hard links and special entries are rejected. No automatic archive repacking. Password-protected archives and unsupported codecs require separate preparation. |
| File filters | Bundled `.flt` presets, masks, regex and filter expressions; install/edit presets | Windows-specific property expressions may have no equivalent. |
| Line/substitution filters | Native engine filters and supported `le:` line-expression transformations | This does not load arbitrary Windows COM/DLL/script plugins. |
| Office/OpenDocument text | Built-in DOCX/XLSX/PPTX/ODT/ODS/ODP extraction, including listed template/macro-enabled XML variants | Text/values only; details below. Sources are read-only. |
| PDF text | Optional Poppler `pdftotext` | No OCR or page bitmap comparison in this converter. |
| Legacy Office | Optional LibreOffice conversion of DOC/XLS/PPT/RTF into a native XML format, then extraction | Requires an installed converter; no Microsoft Office automation or Wine. |
| Rendered webpages | Qt WebEngine panes, navigation, zoom and synchronized scrolling; compare text, HTML and screenshot snapshots | Local pages use relative resources within the selected file's folder; see the [local-content policy](MANUAL.md#rendered-webpage-comparison). No WinWebDiff DOM/resource-event synchronization, network-resource comparison, synchronized form/click replay, or its full automation interface. |
| Projects | WinMerge `.WinMerge` XML, multiple comparisons, relative paths, labels, read-only flags and supported comparison/table options | Path/options records, not a recovery format for unsaved buffers or temporary snapshots. Unsupported fields are reported and retained. |
| Settings migration | MergeDesk INI round trip; supported WinMerge INI and exported `.reg` preferences mapped to native keys | Unknown or incompatible settings are reported and skipped. Import never executes Registry Editor or changes Windows registry keys from the supplied file. |
| Patches/reports | Exact unified patches and self-contained HTML from current editor buffers; HTML/CSV folder reports | Pairwise text patch export; no binary patch format or full WinMerge report/patch-application UI. |
| Theme | System, Light and Dark modes using Qt widgets and OS window decorations | Qt style/platform integration determines how closely controls match a particular OS version. |
| Desktop integration | User-level integrations for supported Windows/Linux file managers | Uses native platform mechanisms; not the original WinMerge Explorer DLL. Installation is explicit. |
| Translations | English default, with the translations included in this build | The entire upstream WinMerge translation catalog is not imported into the new Qt interface. |

## Document converters

The built-in converters read ZIP parts with libarchive and parse XML with Qt.
They do not extract package entries onto the filesystem, execute macros, follow
external package relationships, or write to the original documents.

- **DOCX/DOCM/DOTX/DOTM:** Main document text, tables, tabs and line breaks;
  available headers, footers, footnotes, endnotes and comments follow in named
  sections. Tracked deletions and moved-from text are excluded. Formatting,
  drawings, embedded objects and page layout are outside the extracted view.
- **XLSX/XLSM/XLTX/XLTM:** Worksheets in workbook order. Nonempty cells are
  written as `address<TAB>value`, preserving sparse coordinates. Shared/rich
  strings and inline strings are read. Formulas include their cached values;
  a shared formula reference is identified rather than recalculated. Numeric
  date values remain in their stored representation. Chart sheets and missing
  required relationships produce an error instead of a partial workbook.
- **PPTX/PPTM/POTX/POTM:** Slide text in presentation relationship order, plus
  available notes. Diagram, chart and image content are not rendered.
- **ODT/OTT:** Paragraph text, explicit spaces, tabs, line breaks and tables.
- **ODS/OTS:** Cell coordinates and stored values, including repeated rows
  and cells. Empty repeated regions advance the coordinates without allocating
  a huge blank grid. Populated repetitions are bounded by the output limit.
- **ODP/OTP:** Text grouped by slide. Drawings are not rasterized.
- **PDF:** `pdftotext -layout -enc UTF-8` extracts selectable text. Scanned or
  image-only pages can produce no text; the application reports this. OCR is
  a separate preparation step.
- **DOC/XLS/PPT/RTF:** LibreOffice receives a temporary source copy, private
  output directory and private profile. The profile disables macros and
  active OLE/DDE content. The converted OOXML is read by the native extractor.
  Embedded content and rendering fidelity remain subject to LibreOffice's
  conversion behavior.

Optional helpers are launched through `QProcess` with an argument list, never
a constructed command shell string. The default timeout is 90 seconds. Helper
executables can be set in Tools → Document Conversion Tools; otherwise PATH is
searched. A document conversion fails if a required helper is absent. There
is no silent conversion to an empty successful comparison after a helper error.

The default extracted text limit is 64 MiB. Inputs are limited to 256 MiB;
a ZIP XML part to 32 MiB; selected XML parts together to 128 MiB; and packages
to 20,000 entries. XML DTD/entity declarations, encrypted packages, traversal
names, linked entries, duplicate required parts and malformed required XML
are rejected. These are explicit supported limits, not unrestricted document
size claims. Converted text is temporary UTF-8 and is never saved back into
the Office/PDF container.

## Native file dialogs on GNU/Linux

The Linux startup selects Qt's `xdgdesktopportal` theme before creating the
application when a graphical display and D-Bus session address are present.
Selecting the bundled portal plugin explicitly gives portable packages the
same chooser policy on KDE and GNOME. Qt's portal theme keeps the normal
desktop theme as its base for colors, fonts and other platform behavior.

The package supplies the Qt portal and GTK plugins. The host desktop must
provide `xdg-desktop-portal` and a FileChooser backend appropriate to KDE or
GNOME. The chooser runs through that desktop service, so it does not require
loading the host's KDE Qt plugin into the bundled Qt runtime. When the portal
is unavailable, Qt delegates to its normal desktop base theme, including GTK's
native helper where available, then falls back to a Qt widget dialog.

Explicit `QT_QPA_PLATFORMTHEME` values, including an explicitly empty value,
and Qt's `-platformtheme` command-line option are preserved. Explicit
offscreen/minimal and other non-desktop platform selections are unchanged.
Windows retains Qt's native Windows dialog integration.

The relevant Qt source is
[`qguiapplication.cpp`](https://github.com/qt/qtbase/blob/v6.11.3/src/gui/kernel/qguiapplication.cpp)
and
[`qxdgdesktopportaltheme.cpp`](https://github.com/qt/qtbase/blob/v6.11.3/src/plugins/platformthemes/xdgdesktopportal/qxdgdesktopportaltheme.cpp).

## Qt runtime versions

Release packaging requires the reviewed **Qt 6.11.3** SDK on Windows and
GNU/Linux. The exact version is recorded in
[`packaging/qt-runtime.json`](../packaging/qt-runtime.json); the package tools
reject mismatched SDK versions or missing corresponding license/source
records. The runtime includes security fixes for XML and SVG processing that
are absent from the older Qt 6.4 distribution packages.

Windows x64 requires Windows 10 version 1903 or later, or Windows 11. The
application's UTF-8 process-code-page manifest sets this minimum; see the
[build instructions](BUILDING.md). GNU/Linux packages are built on Debian 12
with a glibc 2.36 baseline and bundle the reviewed Qt SDK. Their relocated
binaries and native chooser integrations must pass the package checks.
Native fonts, dialogs, image codecs and webpage rendering can still vary with
the platform. See [Qt's supported platforms](https://doc.qt.io/qt-6.11/supported-platforms.html),
the [Qt XML advisory](https://www.qt.io/blog/cve-2026-16762), and the
[Qt SVG advisory](https://www.qt.io/blog/security-advisory-cve-2026-8168).

## Projects and settings

A project stores two or three paths per `<paths>` element. Relative filesystem
paths resolve against the project file's directory. HTTP/HTTPS URLs stay URLs.
Windows drive/UNC paths imported on GNU/Linux are retained with a remapping
warning. Unknown nested project elements, including named `unpacker` and
`prediffer` pipelines, are retained as XML and reported; retaining a plugin
name does not execute that plugin. XML projects are limited to 16 MiB and 128
comparisons. DTD declarations are rejected.

Project records can carry the WinMerge whitespace, blank-line, case, EOL,
number, codepage, comment, missing-final-EOL and line-break options, comparison
method, file filter, recursive flag, pane descriptions/read-only flags, window
type, and table delimiter/quote/multiline settings. MergeDesk-specific folder
and output-path records use additional fields. A saved project cannot preserve
an unsaved buffer merely by naming its current file.

Native settings exports include a MergeDesk format marker and preserve Qt
value types such as string lists and byte arrays. Import merges supplied keys;
unmentioned preferences remain unchanged. Restart after import to reload all
cached engine/UI options.

Executable selections for the optional PDF and LibreOffice converters are
intentionally skipped during settings import, including native MergeDesk
exports. Existing helper selections remain unchanged. Configure these programs
explicitly in **Tools → Document Conversion Tools**; ordinary comparison and
interface preferences continue to round-trip through the settings file.

WinMerge migration supports the portable comparison/editor/encoding options,
applicable General preferences, file-backup enablement, CSV/TSV/DSV/image/binary
file-pattern settings, and line/substitution filter records. Type and range
checks prevent invalid settings from being stored under a supported key.
Windows-only fonts/colors, window placement structures, Shell/plugin metadata,
and unrecognized names are reported and skipped. A file containing no supported
settings is rejected without changing preferences.

Registry exports are parsed as text. Supported values are DWORD, quoted strings
and UTF-16 string data (`hex(1)`/`hex(2)`). Only WinMerge's HKCU/HKLM software
branches are considered. Registry deletion directives and unrelated branches
are ignored and reported. Environment variables in expandable strings remain
literal. UTF-8 and BOM-marked UTF-16 files are accepted; legacy ANSI exports
should be re-exported as Unicode. Parsing completes before any settings are
applied, so a malformed Registry file cannot partially import earlier values.

## Export behavior

A unified patch represents exactly one source/destination pair. For three-pane
comparisons the caller chooses the pair. Current editor snapshots include
unsaved edits and their final-newline state. Ignore filters and plugin
transformations do not suppress changes in an exact export.

WinMerge's comparison algorithm is reused, with an export adapter that avoids
the original Windows path conversion and preserves CRLF, LF, lone CR, tabs,
backslashes, UTF-8 bytes and missing-final-newline markers. All supposedly
unchanged spans are checked against the original buffers before writing. The
engine's thread-local options are restored. Output uses atomic replacement and
cannot overwrite a compared source file.

Editor snapshots produce UTF-8 textual patches. Applying them to files stored
in another encoding may require equivalent UTF-8 inputs or deliberate
re-encoding. The low-level path-only exporter accepts non-NUL text bytes;
UTF-16 should first be opened in a text pane. Arbitrary binary patches are
outside this exporter. Text inputs are limited to 256 MiB and 2,000,000 lines per side, and the
generated patch to 512 MiB.

HTML reports escape paths and compared text, contain their own light/dark CSS,
and require no external resources. They present unified changes with context,
not an interactive replica of the comparison window. A legacy non-UTF-8
path-only patch is displayed as ISO-8859-1 with a notice. Folder HTML/CSV reports
require a completed successful scan and describe that scan's comparison rules;
“Identical” under a size/date method does not imply a byte comparison. CSV
formula-like fields are prefixed as literal text for spreadsheet applications.

## Command-line differences

Run `--help` for the implemented WinMerge-style switches. GNU/Linux uses `-`
for switches; the Windows build also recognizes `/`. Noninteractive/exit-code
comparison, pane labels and read-only flags, recursive folder options, window
types, settings-file overrides, and the implemented comparison-rule switches
are available. Automatic merge flags select the destination/ancestor pane.

Some inherited switches remain unsupported: `-or` report output,
`-unpacker`/`-prediffer` named plugin pipelines, and `-clipboard-compare`.
GUI export and document conversion provide the implemented native workflows.
The inherited single-instance/shell/jump-list switches do not implement the
full original Windows behavior. Desktop integration is handled by MergeDesk's
own explicit integration commands. Git/editor integrations should use the
published supported command line rather than assuming every WinMerge switch.

## Validation scope

`app/MergeDeskFeatureTests.cpp` supplies `--selftest-mergedesk-features`.
Its fixtures cover seeded patch reconstruction, CRLF/lone-CR/BOM/Unicode
exports, project/settings round trips, UTF-16 Registry migration, malformed
input rejection and native document extraction. Checks using optional helpers
run when those executables are available. Substitute-helper checks exercise
invocation, timeout and failure handling; validation of a real LibreOffice or
`pdftotext` installation must also record that helper's version and platform.

The GUI, WebEngine rendering, file safety, native dialogs and relocated
packages have separate checks. [VALIDATION.md](VALIDATION.md) describes their
commands and limitations. Results apply to the executable and runtime recorded
for that run; each target platform needs its own evidence. Passing fixtures do
not establish visual document fidelity or complete WinMerge feature parity.
