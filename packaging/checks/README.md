# Repository checks

The **Repository checks** workflow runs on pushes and pull requests to `master`,
and can also be started manually. It complements the native compilation,
application regression tests, and package verification in **Native builds**.
It also runs the Linux packaging safety tests:

```sh
python3 -m unittest discover -s packaging/linux -p test_package.py -v
python3 -m unittest discover -s packaging/release -p test_promote.py -v
```

These reject mixed Qt installations, unaccounted SDK libraries, missing license
or source metadata, and package audits affected by a developer's library path.
The release tests also reject incomplete CI gates, unsafe artifact archives,
incorrect package checksums, missing test evidence, and draft changes during
publication. See [release instructions](../../docs/RELEASING.md).

## Workflow and secret checks

The versions and SHA-256 hashes in [tools.json](tools.json) pin the official
Linux x86-64 releases of:

- [actionlint](https://github.com/rhysd/actionlint), which validates workflow
  syntax, expressions, action inputs, and embedded shell scripts when
  ShellCheck is installed.
- [Gitleaks](https://github.com/gitleaks/gitleaks), which scans the checked-out
  directory for recognizable credentials and other secret patterns. The
  report and console output redact detected secret values.

The workflow installs ShellCheck from Ubuntu's package repository and selects
that executable explicitly. For the same shell and C++ analyzers on Ubuntu
24.04, install `shellcheck` and `cppcheck` with the system package manager.

[install_tools.py](install_tools.py) verifies each downloaded archive before
writing its expected executable. It does not extract arbitrary archive paths,
links, or permissions. To reproduce these checks on Linux x86-64 from the
repository root:

```sh
task_tools_dir="$(mktemp -d)"
python3 packaging/checks/install_tools.py --destination "$task_tools_dir"
"$task_tools_dir/actionlint" -color -shellcheck /usr/bin/shellcheck
"$task_tools_dir/gitleaks" dir . \
  --redact=100 --no-banner --no-color --timeout 300 \
  --max-archive-depth 2 --report-format json \
  --report-path "$task_tools_dir/gitleaks.json"
```

The commands return a failing exit status for findings or scanner errors. The
secret scan covers the current directory, including nested archives up to two
levels; it does not replace a Git history scan. Keep build outputs and local
credentials outside the source tree when reviewing the files intended for
distribution. A clear report only describes the rules and files scanned; it
does not prove the absence of every vulnerability or private detail.

## Application source analysis

The Cppcheck jobs use Ubuntu 24.04's packaged Cppcheck (2.13.0 when this gate was
reviewed), the Qt library model, and C++17. Both Unix and Windows 64-bit
configurations enable warning, performance, and portability checks, and fail
on findings. The Linux pass defines `Q_OS_LINUX` and `Q_OS_UNIX`; the Windows
pass defines `Q_OS_WIN` and `_UNICODE`.

```sh
python3 packaging/checks/run_cppcheck.py --platform unix64
python3 packaging/checks/run_cppcheck.py --platform win64
```

The source list is generated from `app/*.cpp` each time, excluding files named
`*Tests*.cpp` and `main.cpp`, which contains the embedded GUI selftest harness.
The gate has no suppression list. It covers application production translation
units and headers reached from them, not upstream engines or Objective-C++.
Native compilation and behavioral regression tests remain separate gates;
this static analysis does not replace them.

## Workflow credentials and dependencies

Actions are pinned to full commit hashes. Checkouts disable persisted Git
credentials. Build and check jobs request read access to repository contents;
only the release publishing job has contents write permission. Workflows use
the ephemeral repository token and do not use personal access tokens. Review
the matching official release notes and rerun native gates when updating the pins.

These repository checks run as ordinary GitHub Actions jobs. They do not depend
on hosted CodeQL or GitHub secret scanning being enabled; those separate
features must be configured and verified before their results are reported.

Copyright (C) 2026 davoda1. SPDX-License-Identifier: GPL-3.0-or-later.
