# Source provenance and licensing

New MergeDesk work is copyright (C) 2026 davoda1,
licensed under GPL-3.0-or-later. Existing ownership and license notices remain
in force; the fork does not claim authorship of WinMerge or LibreMerge code.

## Adopted source

MergeDesk starts from [LibreMerge](https://github.com/iagodpassos/libremerge)
commit `cb7d5ba48c552cb4643ab8247f4cc692df8f3261` (2026-11-03, “feat(view):
add the syntax highlight menu and status bar column”). LibreMerge's principal
author is Iago Passos. The official [WinMerge downloads page](https://winmerge.org/downloads/)
links LibreMerge as an unofficial cross-platform version. This is a downstream
fork, not a release endorsed or maintained by the WinMerge or LibreMerge teams.

LibreMerge's [vendoring record](https://github.com/iagodpassos/libremerge/blob/cb7d5ba48c552cb4643ab8247f4cc692df8f3261/docs/VENDORED.md)
identifies the WinMerge baseline as
`94a7a3fc5efee0f8b45d08a354299453509467b5` (2026-10-17), with selected newer
files and documented POSIX adaptations. See the retained
[`LibreMerge-VENDORED.md`](upstream/LibreMerge-VENDORED.md) and
[`LibreMerge-CONTRIBUTORS.md`](upstream/LibreMerge-CONTRIBUTORS.md) for the
detailed record and original authors. MergeDesk-specific changes are recorded
in [`VENDORED.md`](VENDORED.md).

## Comparison against WinMerge 2.16.60

The stable release [WinMerge 2.16.60](https://github.com/WinMerge/winmerge/releases/tag/v2.16.60)
was published on 2026-10-27. The GitHub
[baseline-to-release comparison](https://github.com/WinMerge/winmerge/compare/94a7a3fc5efee0f8b45d08a354299453509467b5...v2.16.60)
contains 65 commits. Intersecting its changed files with the vendored `engine/`
files gives these entries:

| Upstream file or submodule | Change after baseline | Adopted LibreMerge state |
| --- | --- | --- |
| `Src/DiffList.cpp` and `.h` | Significant and ignored difference counts, PR #3707 / issue #3649 | Already refreshed to `2f609e631` (2026-11-03), including the later significant-index change #3754 |
| `Src/TreeSitterParser.cpp` and `.h` | Correct overlapping captures, PR #3718 | Already merged from `29ede74ed7ae` (2026-10-23), preserving port adaptations |
| `Externals/winimerge` | WinIMerge updates through 1.0.60 | Portable image core `ImgMergeBuffer.hpp` refreshed from WinIMerge `10e78f722`, which WinMerge pins at `616ab15b7352` (2026-10-25) |

No other file in the adopted portable-engine subset appears in that release's
post-baseline changed-file list. This is a scoped source audit, not a claim
that the Qt application duplicates every Windows GUI, installer, shell,
plugin or translation change in WinMerge 2.16.60. The actual image wrappers
use Qt, and the Windows COM plugin host is intentionally absent. Changes
outside the vendored subset include MFC merge-result UI, splitter and status
bar fixes, translations, 7-Zip packaging, plugin presets, and GNU patch
packaging. `COMPATIBILITY.md` distinguishes these product-level capabilities.

The newer `DiffList` and `TreeSitterParser` files should not be replaced by
older release copies during Windows portability work. All portable patches
should retain the existing adaptation comments and original license headers.

## License compatibility

- The combined application is distributed under **GPL-3.0-or-later**.
- The vendored WinMerge engine is **GPL-2.0-or-later**. Its “or later” grant
  permits distribution in this GPLv3-or-later combination; its original
  copyright and SPDX notices must be retained.
- LibreMerge's Qt GUI and port are **GPL-3.0-or-later**. Their existing author
  credits remain applicable to this fork.
- Qt 6 is available under LGPLv3/GPLv3 or commercial terms. The open-source
  build uses those permitted terms, with the corresponding Qt notices and
  source/relinking obligations appropriate to the shipped binaries.
- GNU diffutils is GPLv2-or-later; xdiff carries LGPLv2.1-or-later notices.
  Tree-sitter and its grammars retain their individual notices. Other linked
  dependencies, including libarchive, ICU, Poco and Boost, retain their own
  licenses. See the upstream dependency records and packaged license notices.
- PDF/legacy Office converters are optional external applications. Their
  installation and redistribution terms are independent of MergeDesk's own
  GPL license. MergeDesk does not bundle proprietary Office components.

Do not replace the original copyright headers with the new fork's name.
Distribute the complete corresponding source of the shipped version, its
build scripts and GPL license alongside any binary distribution or provide
an appropriate source offer. Preserve the contributor acknowledgments,
including the Crystal Edit syntax-parser author credits in About.

Modified adopted source/header files carry a dated MergeDesk modification
notice naming `davoda1`. Retain these notices when making further changes and
update their modification date. These notices identify the downstream changes
and do not replace or transfer the original authors' copyrights.
