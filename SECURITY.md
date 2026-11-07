# Security

MergeDesk opens files, archives, projects, documents, images and webpages that
may come from other people. Security reports should identify the application
version, operating system, Qt runtime version and the affected workflow. Include
a minimal reproducer that contains no personal data or credentials.

Use private vulnerability reporting from the repository's **Security** tab when
it is available. If it is unavailable, open an issue requesting a confidential
reporting channel without including exploit details or sensitive files.

## Runtime dependencies

Release packages use the exact Qt SDK version recorded in
[`packaging/qt-runtime.json`](packaging/qt-runtime.json). Packaging rejects an
SDK that does not match the reviewed version and corresponding license/source
records. Qt and its WebEngine/Chromium dependencies require continuing security
updates; a successful application test run does not establish that a runtime
is free of vulnerabilities.

The application requires Qt 6.11.3 or newer. The release pin includes fixes for
Qt's [XML parser](https://www.qt.io/blog/cve-2026-16762) and
[SVG parser/renderer](https://www.qt.io/blog/security-advisory-cve-2026-8168).
Do not substitute the unsupported Qt WebEngine 6.4 runtime from older
distribution packages. When updating the SDK, review the upstream advisories,
refresh dependency notices and source records, and rerun native and relocated
package validation on both supported platforms.

Other GNU/Linux runtime libraries retain their distribution package and source
versions in the generated package metadata. Windows dependencies use the pinned
vcpkg baseline. Optional PDF and legacy Office converters are external programs;
their installed versions need their own security updates.

## Input handling

Archive comparison extracts regular files and directories into a fresh
temporary directory. It rejects symbolic links, hard links, special entries
and unsafe paths, and reports archive data-integrity errors instead of accepting
damaged contents. Document extraction has byte and entry limits, rejects XML
DTD/entity declarations, and does not overwrite source documents.

Settings imports cannot select executable document converters. Configure those
programs explicitly in **Tools → Document Conversion Tools**. Comparison and
interface preferences remain importable.

Rendered webpage comparisons execute web content through Qt WebEngine. See the
[manual](docs/MANUAL.md) for the local-content access policy and its limits.
Local document resources are confined to the explicitly selected folder; raw
file URLs and outbound network URL requests from those documents are blocked.
These resource restrictions complement Chromium's security model; they are not
a separate operating-system or complete network sandbox.
The application's normal launch does not disable Chromium's sandbox. Headless
CI overrides are restricted to isolated test jobs and are not distributed as
application defaults.

## Automated checks

[Repository checks](packaging/checks/README.md) and
[native validation](docs/VALIDATION.md) complement source review. Their reports
apply to the revision, platform and rules actually tested. A secret-pattern
scan cannot replace review of screenshots, documentation, generated metadata
and other files intended for publication.

Copyright (C) 2026 davoda1. SPDX-License-Identifier: GPL-3.0-or-later.
