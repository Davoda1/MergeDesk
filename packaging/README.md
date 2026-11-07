# MergeDesk packaging

MergeDesk 1.0.0 releases target Windows x64 and GNU/Linux x86-64. The
[Native builds workflow](../.github/workflows/build.yml) is the canonical
build and validation configuration. See the [build guide](../docs/BUILDING.md)
for dependencies and supported commands.

- [Windows packaging](windows/package.ps1) creates the portable ZIP.
- [GNU/Linux packaging](linux/package.py) creates the portable TAR.GZ, DEB,
  and AppImage.
- The [release workflow](../.github/workflows/release.yml) uses the
  [promotion tool](release/promote.py) to publish validated packages. See the
  [release guide](../docs/RELEASING.md) for its requirements.

## Retained upstream references

The following LibreMerge packaging material is preserved for upstream
reference only and is outside the supported MergeDesk release process:

- The [AUR recipes](aur/).
- The macOS helpers and resources, including [make_dmg.sh](make_dmg.sh),
  [build_deps.sh](build_deps.sh), and [Info.plist.in](Info.plist.in).
- The legacy AppImage helpers [make_appimage.sh](make_appimage.sh),
  [appimage_in_docker.sh](appimage_in_docker.sh), and
  [linux/build_qt.sh](linux/build_qt.sh), together with the original
  [LibreMerge desktop entry](linux/libremerge.desktop),
  [AppStream metadata](linux/io.github.iagodpassos.libremerge.appdata.xml),
  and LibreMerge icons.

Their original project names, versions, authors, URLs, and platform details
describe LibreMerge; they do not identify MergeDesk distributions or change
MergeDesk's version or supported platforms. Original third-party notices
remain applicable. See [source provenance](../docs/PROVENANCE.md).

Copyright (C) 2026 davoda1. SPDX-License-Identifier: GPL-3.0-or-later.
