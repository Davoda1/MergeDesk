# MergeDesk 1.0.0

**Native Qt6 file and folder comparison for Windows and GNU/Linux.**

Copyright (C) 2026 davoda1. GPL-3.0-or-later. MergeDesk builds on the WinMerge comparison engines and the LibreMerge Qt interface. Original upstream notices are retained in Third-party notices. WinMerge 2.16.60 is the functional reference.

## Start a comparison

Choose **File → Open** or press **Ctrl+O**. Select two or three files or folders. The Browse buttons use your operating system's file dialogs. You can type paths, paste them, use recent paths, or drag items onto the window. Swap buttons change their order. A read-only check box protects an input pane from edits and merging.

**File → New** opens an empty text comparison for pasting text. Opening one file compares it with a temporary snapshot of itself so that you can review edits. Open comparisons remain in separate tabs.

Text, image, table, and binary modes are selected from the file types. Archives are opened as folders. You can also select the comparison type on the command line. Modern Office and OpenDocument files open as extracted text; their original documents are read-only.

## Text comparisons and merging

The panes are aligned line by line. Gray filler rows are display-only and never become part of a saved file. Gold marks differences; the selected difference has a stronger color. Small highlights identify changed characters or words. The location pane shows the overall distribution of differences, while the difference pane shows the selected block in detail.

Use the first, previous, next, and last difference commands to navigate. Click in the location pane to jump. The Merge menu and toolbar copy a difference to the left or right, copy and advance, or copy all differences. In a three-way comparison, commands act relative to the active pane. Right-click a pane for the available directions and selection-based commands.

You can edit a writable pane normally. Undo and redo cover edits and merges. Recompare with **F5** after changing the comparison rules or when you want to refresh explicitly. **Ctrl+F** finds text; **Ctrl+H** replaces it; **Ctrl+G** goes to a line or difference. **F4** selects the next difference within a line.

**Merge → Auto Merge** combines two variants using the destination pane's current contents, including unsaved edits, as the common ancestor. Choose the correct ancestor/destination; it is commonly the middle pane. Independent changes are combined. Conflicting changes remain between `<<<<<<< LEFT`, `||||||| BASE`, `=======`, and `>>>>>>> RIGHT` markers for manual resolution. Auto merge modifies the editor, not the disk; review it and save when ready.

## Save files safely

**Ctrl+S** saves modified panes. Untitled panes ask for a filename. Closing a modified comparison offers Save, Discard, and Cancel. Files changed by another program are checked before reload or overwrite.

Text, table, binary, and supported image writes use an intermediate file and atomic replacement. Direct-write fallback is disabled: a failed write must not truncate the original. A text encoding that cannot represent newly entered characters produces an error instead of silently substituting them.

Text saving preserves the current encoding and BOM. Click a text pane's status-bar encoding label to choose UTF-8, UTF-8 with BOM, or UTF-16 LE/BE with BOM for the next save. This changes the output encoding without reinterpreting the text or changing its line endings, and the choice can be undone. Read-only panes disable these choices.

**File → Convert Line Endings to** deliberately converts the active pane. Options → Editor controls preservation of mixed original line endings. Clicking the status-bar line-ending label opens the same conversion menu.

Backups are enabled by default for file comparisons. `<filename>.bak` holds the most recent pre-save contents; an existing backup is retained under a dated additional name before that name is refreshed. Writing or copying through symbolic links is refused. This prevents a comparison of one path from unexpectedly overwriting another path.

## Folder comparisons

Select two or three directories. **Include subfolders** determines whether the entire tree is scanned. Scans run in the background with progress and cancellation. The result distinguishes identical, different, unique, skipped, binary, and error items. Three-way comparisons also identify which pair remains identical.

Choose a tree or flat view. Use the View menu or the display filter bar to limit the visible results. Comparison methods include full contents, quick contents, binary contents, modification time, time and size, size, and existence. Choose full contents when you need an actual content comparison.

Double-click a file row to open its comparison. A file present only on one side opens against an empty pane. Multi-selection and the context menu support other comparison pairings. Saving an opened comparison updates its folder row.

Copy actions ask for confirmation showing the selected item count and copy direction. Destination files are atomically replaced with backup copies. Symbolic links are not followed during copies. Deletion uses the operating system's Trash or Recycle Bin; a failed trash operation is reported rather than silently changed into permanent deletion. File and folder type conflicts require explicit resolution.

## Filters and comparison options

**Tools → Filters** contains file filters, line filters, and substitution filters. File filters can use masks, regular expressions, and WinMerge-style expressions. Bundled preset `.flt` files can be selected, edited, and supplemented with your own.

Line filters classify matching changes as ignored differences. Substitution filters normalize matching patterns for comparison. These rules affect the comparison; they do not silently rewrite the original documents.

Options → Compare controls whitespace, case, empty lines, line endings, numbers, encoding differences, and comments. Select Myers, minimal, patience, or histogram comparison as appropriate. Moved-block detection and syntax-aware comment handling are available. Exports intended as patches use exact differences so they remain applicable.

## Tables, images, and binary files

**Tables:** CSV, TSV, and configured delimited formats open as editable grids. Quoted fields and multiline cells are supported. The Table options control delimiter, quote character, file patterns, and multiline handling. Ignore a column through its header menu. Copy differences, selected cells, or selected rows between panes and undo changes.

**Images:** Pixel/block comparisons support adjustable thresholds, overlays, blink, zoom, rotation, flips, selection, and copying differences between panes. The image comparison engine comes from WinIMerge. Supported decoding and encoding formats depend on the bundled Qt image plugins. If the original format cannot be written safely, Save As offers PNG; animated or multipage data is not silently flattened into the original file.

**Binary:** The hex editor shows the files side by side, highlights changed bytes, and supports difference navigation, insert/overwrite, selection-based copying, search/replace, go to offset, and undo. Review changes before saving.

## Archives and document formats

ZIP, 7z, TAR and supported compressed variants extract into temporary directories and open as folder comparisons, including archive-versus-directory and nested archives. Only ordinary files and directories are extracted. Symbolic links, hard links, special entries and unsafe paths are rejected. These comparisons do not automatically repack a modified archive. Export or copy the wanted files explicitly.

DOCX, XLSX, PPTX, ODT, ODS, and ODP use a built-in text/value extraction path. It compares document contents, not page layout, fonts, formulas' calculated appearance, or embedded objects. Extracted documents are clearly labeled and opened read-only. Password-protected or malformed files are rejected.

PDF text extraction uses **pdftotext**. Old binary Office formats use **LibreOffice**. Install these optional tools separately if they are needed. **Tools → Document Conversion Tools** accepts explicit executable paths; an empty field searches PATH. Converters run without a command shell, have a timeout, use temporary output, and do not modify the source documents. Scanned PDFs require OCR outside MergeDesk.

## Rendered webpage comparison

Choose **File → Compare Webpages** and enter two or three HTTP/HTTPS URLs or local HTML paths. The panes render through Qt WebEngine. Their back, forward, reload, and address controls are independent; synchronized scrolling can be switched off. Zoom controls apply to all panes.

Compare text or HTML opens a normal text comparison of snapshots of the loaded pages. Compare screenshots opens the rendered visible areas in image comparison. Full pages captures a larger page up to a bounded pixel budget; use visible-area captures for very large pages. Snapshot comparisons are read-only and survive closing the originating webpage tab for the rest of the application session.

Each pane uses a separate temporary browser profile, with memory cache and no persistent cookies. Camera, microphone, and location access are not granted. HTTP/HTTPS pages load their normal web resources. No account is required for the application. Page changes, animation, authentication, and responsive layout can naturally affect a live-page comparison.

Opening a local HTML file allows relative resources within that file's folder, including stylesheets, images, scripts and JSON used by page scripts or workers. Resources outside that folder, symbolic links that escape it, raw `file://` resource URLs and outbound network URL requests are blocked. Use relative resource paths for local pages; references to parent folders or absolute `file://` paths will not load. Enter another local path in the address field to choose a different folder. Back and Forward retain each document's original folder boundary. The address fields and saved projects continue to use normal local file addresses.

## Projects, settings, patches, and reports

**File → Save Project** records open comparisons as a WinMerge-compatible XML project. Projects store paths and supported options, not unsaved editor contents. Save documents separately before relying on a project to reopen them. Relative paths make projects easier to move. Imported fields that have no equivalent are reported and retained when the imported comparison is saved again.

**Tools → Import Settings / Export Settings** transfers supported settings through an INI file. Imported settings are merged; restart to reload all cached options. Converter executable selections are skipped during import, and existing selections are retained. Set PDF/LibreOffice helpers explicitly in **Tools → Document Conversion Tools**. Windows settings that have no portable meaning require review.

**Tools → Create Patch** exports an exact unified patch from the current text or table buffers, including unsaved edits and final-newline differences. For a three-pane comparison, choose the pair to export. HTML reports are self-contained, escaped, and suitable for sharing. Folder comparisons can export HTML or CSV reports after their scan finishes.

## Appearance and languages

Options → General contains **System**, **Light**, and **Dark** themes. The application uses consistent Qt widgets on both platforms and retains operating-system window decorations and native file dialogs. System follows the OS/desktop appearance where provided. Changing the theme updates open comparison views.

On GNU/Linux, graphical sessions with a D-Bus session address prefer the desktop portal for file dialogs. KDE and GNOME use their installed FileChooser portal backend; the package includes the Qt portal and GTK plugins, while the desktop provides the portal service. If the portal is unavailable, Qt falls back to the desktop's normal dialog implementation, or its widget dialog when no native helper is available. An explicit `QT_QPA_PLATFORMTHEME` setting (including an empty value) or Qt `-platformtheme` option takes precedence. Offscreen/minimal runs retain their normal platform behavior.

English is the default interface language. Hungarian, German, and French are available; restart after changing the language. Text fonts, tab width, tabs/spaces, syntax coloring, line differences, whitespace, and line-ending display are configurable. Window size and position are remembered. The main window does not scroll as a single page; comparison panes and individual settings panels scroll when needed.

## Desktop and command-line integration

**Tools → Desktop Integration** installs or removes optional user-level integration. Windows Explorer, KDE Dolphin, GNOME Files/Nautilus, Nemo, and Thunar are supported through their platform mechanisms. Some managers require a restart or an optional extension package. Portable application paths must remain valid; move the application first, then reinstall its integration.

Common command-line examples:

```
mergedesk left.txt right.txt
mergedesk -r folderA folderB
mergedesk -t image before.png after.png
mergedesk -t webpage https://example.org/a https://example.org/b
mergedesk -wl -wr -dl Original -dr Updated old.txt new.txt
mergedesk -noninteractive -enableexitcode left.txt right.txt
mergedesk comparison.WinMerge
```

Use `MergeDesk.exe` on Windows. Windows also accepts the usual `/` switch prefix. GNU/Linux uses `-` so absolute POSIX paths remain unambiguous. Run `--help` for supported switches. Exit-code mode returns 0 for identical, 1 for different, and 2 for an error. Configure Git or another tool to launch the executable with its file arguments; no shell wrapper is required.

## Compatibility and licenses

The feature and platform matrix in `docs/COMPATIBILITY.md` records differences from the WinMerge reference, including portable replacements for Windows-only components. Unmodified third-party WinMerge DLL/COM plugins are not a cross-platform plugin ABI. Built-in transformations, filters, archive handling, and document converters cover the supported equivalents.

The About dialog identifies **davoda1** as the author of MergeDesk work and modifications, credits WinMerge and LibreMerge, and opens the complete GPL and retained third-party notices. The full source and build scripts accompany the release. No telemetry or automatic update service is included.
