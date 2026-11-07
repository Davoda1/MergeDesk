# Publishing a release

The [Release packages workflow](../.github/workflows/release.yml) promotes the
packages already produced by [Native builds](../.github/workflows/build.yml).
It publishes a Windows portable ZIP and GNU/Linux tar, DEB, and AppImage,
with a single `SHA256SUMS` manifest covering all four packages. It does not
rebuild the application.

## Prepare the version

Keep the application, CMake, Windows resource, package, and documentation
versions consistent. A stable tag such as `v1.0.0` must match the tagged CMake
project version and both platforms' package metadata. The Qt runtime and its
notices must match [the reviewed runtime pin](../packaging/qt-runtime.json).

Push the intended release commit to `master`. Confirm that both **Native
builds** and **Repository checks** pass for that exact commit. The release
gate requires all three native jobs and all three repository jobs:

| Workflow | Required jobs |
| --- | --- |
| Native builds | Windows MSVC, Ubuntu native tests, Debian 12 package builds and installation |
| Repository checks | Workflow/secret checks and packaging/release regressions, Cppcheck Unix, Cppcheck Windows |

Selective manually dispatched builds do not qualify. The gate uses the latest
`master` push run for each workflow, so a newer failed or running run cannot
be replaced with an earlier green result.

## Publish the tested commit

With the reviewed commit checked out, create and push its version tag:

```sh
git tag v1.0.0
git push origin v1.0.0
```

The tag starts **Release packages**. The workflow can also be dispatched from
`master` with an existing version tag. It waits up to two hours for the exact
commit's required gates and then fails if they are still missing or incomplete.
It never creates or moves the tag.

The read-only verification job uses promotion code from `master`. Its
immutable automation commit is reused by the publishing job. Tagged source is
read as data; scripts from an arbitrary requested tag are not executed by the
publisher.

Before creating a release, promotion checks:

- The exact repository, source SHA, workflow identity, complete job results,
  unexpired artifacts, and GitHub artifact digests.
- All four package checksums, their exact filenames, the application version,
  Qt version, source revision, platform, and Debian 12 baseline.
- Passing Windows build-tree and portable tests, including all four language
  catalogs, and the complete Linux package test set.
- KDE and GNOME chooser evidence from Ubuntu, the portable Linux runtime, and
  the installed DEB, plus a clear secret-scan report.
- Matching embedded Windows ZIP and Linux tar build/test reports and the
  tagged AppImage tool pins.

Artifact archives are read through an exact file allowlist with size bounds.
Promotion never runs a packaged executable or installer.

The native build artifacts retain per-package checksum files for validation.
Promotion combines the verified package hashes into one `SHA256SUMS` manifest
with exact filenames, sorted entries, and LF line endings.

After validation, the workflow creates a draft and uploads only the four
packages and the single manifest. It downloads every uploaded asset again,
including `SHA256SUMS`, and verifies its hash and size. It then rechecks the
tag, CI runs, draft metadata, and complete asset set before publishing.
Release notes use a controlled public-facing template with `davoda1`
attribution and the GPL-3.0-or-later license.

## Failure and retry behavior

Failed CI, missing reports, failed tests, mismatched checksums, or invalid
metadata stop promotion before any release is created. A failure during
upload or download verification leaves an unpublished draft for inspection.
An existing release is never overwritten automatically.

For an interrupted upload, inspect the draft and remove that unfinished draft
before dispatching **Release packages** again with the same unchanged tag.
Retain the tag when removing an unfinished draft. If source changes are
needed, push and validate the corrected version before creating its release
tag. Do not move tags attached to published releases.

Artifacts must still be available when promoting. The current secret-report
retention is 14 days and native package retention is 30 days. Regenerate the
complete master push gates if their artifacts have expired.

## Credentials and local regression tests

The workflow uses only the repository-scoped, short-lived `GITHUB_TOKEN`.
Verification has `contents: read` and `actions: read`; only the publishing
job has `contents: write`. Checkouts do not retain credentials. Downloads
follow approved HTTPS storage redirects without forwarding the bearer token
to another host.

The offline regression suite uses a fake GitHub API and real archive/checksum
parsing. It needs no credentials or network access:

```sh
python3 -m unittest discover -s packaging/release -p test_promote.py -v
```

These tests cover failed/skipped/incomplete gates, wrong source revisions,
missing tests and localizations, unsafe archive entries, corrupt downloads,
upload failures, retagging, and changes made to a draft during verification.
The repository checks run them on every push and pull request to `master`.

Implementation references:
[GitHub workflow runs API](https://docs.github.com/en/rest/actions/workflow-runs),
[artifact API](https://docs.github.com/en/rest/actions/artifacts), and
[release assets API](https://docs.github.com/en/rest/releases/assets).

Copyright (C) 2026 davoda1. SPDX-License-Identifier: GPL-3.0-or-later.
