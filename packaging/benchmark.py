#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 davoda1
"""Reproducible, bounded smoke measurements of an actual MergeDesk executable.

Usage:
  python3 packaging/benchmark.py --executable build/app/mergedesk \
      --output qa/benchmark-results.json --brief qa/benchmark-brief.md

Fixtures are deterministic and temporary by default; --keep-inputs retains them.
Each comparison has a separate, empty settings/cache/data directory. Timings
include process startup, comparison and shutdown, but exclude fixture generation
and input hashing. This is one smoke sample per workload, not a statistical
benchmark or a comparison with another product. Each process is stopped after
at most 60 seconds. Pillow is optional, and only used to create image fixtures.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any

SUITE_VERSION = 1
TEXT_LINES = 100_000
TEXT_CHANGES = tuple(1_000 + i * 1_200 for i in range(80))
TABLE_ROWS = 20_000
TABLE_CHANGES = frozenset(137 + i * 307 for i in range(64))
MAX_OUTPUT = 16_384


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def input_manifest(root: Path) -> dict[str, Any]:
    """Check bytes, filenames, file permissions and mtime (not read-induced atime)."""
    records = []
    directories = []
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        info = path.lstat()
        if stat.S_ISDIR(info.st_mode):
            directories.append(relative)
        elif stat.S_ISREG(info.st_mode):
            records.append({"path": relative, "sizeBytes": info.st_size,
                            "sha256": sha256_file(path), "mtimeNs": info.st_mtime_ns,
                            "permissions": stat.S_IMODE(info.st_mode)})
        else:
            # Fixtures never contain links or special files; new ones are mutations.
            records.append({"path": relative, "unexpectedFileType": info.st_mode})
    return {"files": records, "directories": directories}


def manifest_summary(manifest: dict[str, Any]) -> dict[str, Any]:
    # Do not include timestamps in the reproducible content identity.
    contents = [{key: value for key, value in record.items()
                 if key not in ("mtimeNs", "permissions")}
                for record in manifest["files"]]
    serialized = json.dumps({"files": contents, "directories": manifest["directories"]},
                            sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {"fileCount": len(contents), "directoryCount": len(manifest["directories"]),
            "totalBytes": sum(record.get("sizeBytes", 0) for record in contents),
            "contentManifestSha256": hashlib.sha256(serialized).hexdigest()}


def mutation_paths(before: dict[str, Any], after: dict[str, Any]) -> list[str]:
    old = {item["path"]: item for item in before["files"]}
    new = {item["path"]: item for item in after["files"]}
    changed = [name for name in sorted(old.keys() | new.keys())
               if old.get(name) != new.get(name)]
    changed.extend(name + "/" for name in sorted(set(before["directories"])
                                                ^ set(after["directories"])))
    return changed


def isolated_environment(root: Path) -> dict[str, str]:
    environment = os.environ.copy()
    for key, name in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                      ("XDG_DATA_HOME", "data"), ("XDG_RUNTIME_DIR", "runtime")):
        path = root / name
        path.mkdir(parents=True, exist_ok=True)
        path.chmod(0o700)
        environment[key] = str(path)
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["MERGEDESK_LANGUAGE"] = "en_US"
    return environment


def run_process(command: list[str], environment: dict[str, str], work: Path,
                timeout: float) -> dict[str, Any]:
    """Use wait4 for this particular child, not cumulative RUSAGE_CHILDREN."""
    work.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {"command": command, "timeoutSeconds": timeout,
                              "exitCode": None, "elapsedSeconds": None,
                              "peakRssBytes": None, "peakRssSource": None,
                              "timedOut": False}
    with (work / "stdout.txt").open("w+b") as output, (work / "stderr.txt").open("w+b") as errors:
        started = time.perf_counter()
        try:
            process = subprocess.Popen(command, cwd=work, env=environment,
                                       stdout=output, stderr=errors,
                                       start_new_session=(os.name == "posix"))
        except OSError as error:
            result["elapsedSeconds"] = round(time.perf_counter() - started, 6)
            result["launchError"] = str(error)
            return result
        def stop_process() -> None:
            try:
                if os.name == "posix":
                    os.killpg(process.pid, signal.SIGKILL)
                else:
                    process.kill()
            except ProcessLookupError:
                pass

        usage = None
        try:
            if hasattr(os, "wait4"):
                while True:
                    pid, status, child_usage = os.wait4(process.pid, os.WNOHANG)
                    if pid:
                        process.returncode = os.waitstatus_to_exitcode(status)
                        usage = child_usage
                        break
                    if time.perf_counter() - started >= timeout:
                        result["timedOut"] = True
                        stop_process()
                        _, status, usage = os.wait4(process.pid, 0)
                        process.returncode = os.waitstatus_to_exitcode(status)
                        break
                    time.sleep(0.02)
            else:
                try:
                    process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    result["timedOut"] = True
                    stop_process()
                    process.wait(timeout=5)
        except BaseException:
            # Ctrl-C or a collection error must not leave a comparison running.
            stop_process()
            process.wait(timeout=5)
            raise
        result["elapsedSeconds"] = round(time.perf_counter() - started, 6)
        result["exitCode"] = process.returncode
        if usage is not None:
            result["cpuUserSeconds"] = round(usage.ru_utime, 6)
            result["cpuSystemSeconds"] = round(usage.ru_stime, 6)
            if sys.platform.startswith("linux"):
                result["peakRssBytes"] = int(usage.ru_maxrss * 1024)
                result["peakRssSource"] = "os.wait4 child ru_maxrss; Linux KiB converted to bytes"
            elif sys.platform == "darwin":
                result["peakRssBytes"] = int(usage.ru_maxrss)
                result["peakRssSource"] = "os.wait4 child ru_maxrss; macOS bytes"
        for label, stream in (("stdout", output), ("stderr", errors)):
            stream.seek(0)
            data = stream.read(MAX_OUTPUT + 1)
            result[label] = data[:MAX_OUTPUT].decode("utf-8", errors="replace")
            result[label + "Truncated"] = len(data) > MAX_OUTPUT
    return result


def make_text(root: Path) -> dict[str, Any]:
    root.mkdir(parents=True)
    paths = [root / "left.txt", root / "right.txt"]
    edits = set(TEXT_CHANGES)
    with paths[0].open("wb") as left, paths[1].open("wb") as right:
        for number in range(TEXT_LINES):
            line = (f"record {number:06d} | group {number % 97:02d} | value {number * 37:012d}"
                    " | text comparison fixture alpha beta\n").encode("ascii")
            left.write(line)
            right.write(line.replace(b"alpha", b"omega") if number in edits else line)
    return {"name": "text", "title": "100,000-line text", "paths": paths,
            "arguments": ["--selftest-count"], "expectedExitCode": 0,
            "expectedDiffCount": len(edits), "fixture": {
                "linesPerFile": TEXT_LINES, "encoding": "UTF-8 (ASCII subset), LF",
                "bytesPerFile": paths[0].stat().st_size,
                "changedLinesOneBased": [number + 1 for number in TEXT_CHANGES],
                "expectedSeparateDifferenceBlocks": 80}}


def make_folders(root: Path) -> dict[str, Any]:
    paths = [root / "left", root / "right"]
    edits = {19 + 37 * i for i in range(25)}
    for side, directory in enumerate(paths):
        for number in range(1_000):
            subdirectory = directory / f"group-{number // 50:02d}" / f"set-{number // 10 % 5}"
            subdirectory.mkdir(parents=True, exist_ok=True)
            prefix = "item" if number < 990 else ("left-only" if side == 0 else "right-only")
            content = (f"fixture={number:04d}\ncategory={number % 13}\n"
                       + "stable text for recursive folder comparison\n" * 12)
            if side == 1 and number in edits:
                content += "changed in the right input\n"
            (subdirectory / f"{prefix}-{number:04d}.txt").write_text(content, encoding="utf-8", newline="\n")
    return {"name": "folders", "title": "Recursive 1,000-file folders", "paths": paths,
            "arguments": ["-noninteractive", "-enableexitcode", "-noprefs", "-r"],
            "expectedExitCode": 1, "fixture": {
                "filesPerSide": 1_000, "commonPaths": 990, "changedCommonFiles": len(edits),
                "leftOnlyFiles": 10, "rightOnlyFiles": 10,
                "nestedDirectoriesPerSide": 120, "depthBelowRoot": 2,
                "expectedResult": "different"}}


def make_table(root: Path) -> dict[str, Any]:
    root.mkdir(parents=True)
    paths = [root / "left.csv", root / "right.csv"]
    for side, path in enumerate(paths):
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream, lineterminator="\n")
            writer.writerow(["id", "region", "name", "status", "units", "price", "note", "revision"])
            for number in range(TABLE_ROWS):
                units = number % 500 + (1 if side == 1 and number in TABLE_CHANGES else 0)
                writer.writerow([number, f"region-{number % 12:02d}", f"Item {number:05d}",
                                 "active", units, f"{number % 2000}.{number % 100:02d}",
                                 f"batch {number % 100}, verified", "baseline"])
    return {"name": "table", "title": "20,000-row CSV", "paths": paths,
            "arguments": ["-noninteractive", "-enableexitcode", "-noprefs", "-t", "table"],
            "expectedExitCode": 1, "fixture": {
                "dataRowsPerFile": TABLE_ROWS, "headerRowsPerFile": 1, "columns": 8,
                "changedDataRows": len(TABLE_CHANGES), "delimiter": ",",
                "encoding": "UTF-8, LF; quoted fields contain commas",
                "expectedResult": "different"}}


def make_image(root: Path) -> dict[str, Any]:
    try:
        import PIL
        from PIL import Image, ImageDraw
    except ImportError:
        return {"name": "image", "title": "2.07-megapixel PNG", "status": "skipped",
                "reason": "Optional Pillow package is not installed; other workloads still run."}
    root.mkdir(parents=True)
    paths = [root / "left.png", root / "right.png"]
    size = (1920, 1080)
    gradient = Image.linear_gradient("L")
    red = gradient.transpose(Image.Transpose.ROTATE_90).resize(size)
    green = gradient.resize(size)
    picture = Image.merge("RGB", (red, green, Image.new("L", size, 105)))
    draw = ImageDraw.Draw(picture)
    for number in range(16):
        x, y = number * 113, (number * 71) % 800
        draw.rectangle((x, y, x + 70, y + 130), outline=(230, 235, 240), width=3)
    picture.save(paths[0], format="PNG")
    ImageDraw.Draw(picture).rectangle((724, 396, 851, 491), fill=(240, 37, 86))
    picture.save(paths[1], format="PNG")
    return {"name": "image", "title": "2.07-megapixel PNG", "paths": paths,
            "arguments": ["-noninteractive", "-enableexitcode", "-noprefs", "-t", "image"],
            "expectedExitCode": 1, "fixture": {
                "width": size[0], "height": size[1], "pixelsPerFile": size[0] * size[1],
                "mode": "RGB", "format": "PNG", "content": "synthetic gradient and outlined rectangles",
                "editedRectangle": {"x": 724, "y": 396, "width": 128, "height": 96},
                "pillowVersion": PIL.__version__, "expectedResult": "different"}}


def environment_details(executable: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "platform": platform.platform(), "system": platform.system(),
        "machine": platform.machine(), "kernel": platform.release(),
        "pythonVersion": platform.python_version(), "logicalCpuCount": os.cpu_count(),
        "qtPlatform": "offscreen", "settingsIsolation": "separate XDG config/cache/data/runtime per run",
        "locale": "MERGEDESK_LANGUAGE=en_US", "buildType": None,
    }
    try:
        result["operatingSystem"] = platform.freedesktop_os_release().get("PRETTY_NAME")
    except (OSError, AttributeError):
        pass
    if sys.platform.startswith("linux"):
        try:
            cpu = Path("/proc/cpuinfo").read_text()
            match = re.search(r"^model name\s*:\s*(.+)$", cpu, re.MULTILINE)
            if match:
                result["cpuModel"] = match.group(1)
            memory = Path("/proc/meminfo").read_text()
            match = re.search(r"^MemTotal:\s*(\d+) kB$", memory, re.MULTILINE)
            if match:
                result["visibleSystemMemoryBytes"] = int(match.group(1)) * 1024
        except OSError:
            pass
        for file, key in (("memory.max", "cgroupMemoryLimit"), ("cpu.max", "cgroupCpuQuota")):
            try:
                result[key] = Path("/sys/fs/cgroup", file).read_text().strip()
            except OSError:
                pass
    cache = executable.parent.parent / "CMakeCache.txt"
    if cache.is_file():
        contents = cache.read_text(errors="replace")
        match = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", contents, re.MULTILINE)
        if match:
            result["buildType"] = match.group(1)
        match = re.search(r"^Qt6_DIR:PATH=(.*)$", contents, re.MULTILINE)
        if match:
            for filename in ("Qt6ConfigVersion.cmake", "Qt6ConfigVersionImpl.cmake"):
                version_file = Path(match.group(1)) / filename
                try:
                    version = re.search(r'set\(PACKAGE_VERSION "([^"]+)"\)', version_file.read_text())
                    if version:
                        result["qtPackageVersion"] = version.group(1)
                        result["qtVersionSource"] = "CMakeCache Qt6_DIR/" + filename
                        break
                except OSError:
                    pass
    return result


def write_results(output: Path, brief: Path, report: dict[str, Any]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    brief.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    lines = ["# MergeDesk performance smoke run", "", f"Recorded: {report['recordedUtc']}", "",
             f"**Overall result: {report['status']}.** One process invocation per workload. "
             "These are local smoke measurements, not general speed or capacity claims.", "",
             f"Executable: `{report['executable']['path']}`", "",
             f"Reported version: `{report['executable'].get('reportedVersion', 'unavailable')}`", "",
             f"Environment: {report['environment']['platform']}; "
             f"build type {report['environment'].get('buildType') or 'not recorded'}; "
             "Qt offscreen platform.", "",
             "| Workload | Input per side | Result | Elapsed seconds | Child peak RSS | Inputs unchanged |",
             "| --- | --- | --- | ---: | ---: | --- |"]
    for workload in report["workloads"]:
        fixture = workload.get("fixture", {})
        name = workload["name"]
        size = {"text": f"100,000 lines; {fixture.get('bytesPerFile', 0):,} bytes",
                "folders": "1,000 files; 120 subdirectories",
                "table": "20,000 data rows × 8 columns",
                "image": "1,920 × 1,080 RGB PNG"}.get(name, "—")
        elapsed = workload.get("elapsedSeconds")
        memory = workload.get("peakRssBytes")
        elapsed_text = f"{elapsed:.3f}" if elapsed is not None else "—"
        memory_text = f"{memory / (1024 * 1024):.1f} MiB" if memory is not None else "unavailable"
        unchanged = str(workload.get("inputsUnchanged", "not run")).lower()
        lines.append(f"| {workload['title']} | {size} | {workload['status']} | "
                     f"{elapsed_text} | {memory_text} | {unchanged} |")
    lines += ["", "## What was checked", "",
              "- Text: the application's `--selftest-count` must print exactly `diffs: 80` and exit 0.",
              "- Folder/table/image: the normal unattended CLI must return 1 (different), with "
              "preferences disabled. Folder scanning is explicitly recursive.",
              "- Every fixture's filenames, bytes, file permissions and modification times are "
              "compared before and after. The JSON records reproducible content-manifest SHA-256 values.",
              "- A separate settings/cache/data directory is used for each invocation.", "",
              "## Reading the measurements", "",
              "Elapsed time includes application startup, comparison and shutdown, but excludes "
              "fixture generation and validation. Only one sample is taken; filesystem caches, "
              "CPU scheduling and concurrent work are not controlled. The synthetic inputs are "
              "described completely in the JSON. They do not establish maximum supported file sizes.", "",
              "On Linux, peak RSS comes from `wait4` for that particular child, converted from KiB. "
              "It is a process high-water mark, not the sum of a process tree or the Python fixture "
              "generator's memory. It is unavailable where the operating system does not expose it. "
              "Polling adds up to about 20 ms to the elapsed measurement.", "",
              f"Per-comparison timeout: {report['timeoutSeconds']:g} seconds. A timeout, launch error, "
              "unexpected exit code/count, or input mutation fails that workload. The optional image "
              "workload is skipped if Pillow is unavailable.", ""]
    problems = [item for item in report["workloads"] if item["status"] != "passed"]
    if problems:
        lines += ["## Incomplete or failed workloads", ""]
        for item in problems:
            reason = item.get("reason") or item.get("launchError") or "; ".join(item.get("failures", []))
            lines.append(f"- **{item['title']} — {item['status']}:** {reason or 'See JSON diagnostics.'}")
        lines.append("")
    lines += ["## Reproduce", "", "```sh",
              "python3 packaging/benchmark.py --executable /path/to/mergedesk \\",
              "  --output qa/benchmark-results.json --brief qa/benchmark-brief.md",
              "```", "", "Use `--keep-inputs` to retain generated files and full subprocess logs. "
              "The JSON includes the exact executable SHA-256 and invocation arguments.", ""]
    brief.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--executable", required=True, type=Path, help="Actual built MergeDesk executable")
    parser.add_argument("--output", required=True, type=Path, help="JSON results path")
    parser.add_argument("--brief", type=Path, help="Readable Markdown summary (defaults beside JSON)")
    parser.add_argument("--timeout", type=float, default=45.0, help="Seconds per workload, maximum 60 (default 45)")
    parser.add_argument("--keep-inputs", action="store_true", help="Keep deterministic fixtures and child logs")
    parser.add_argument("--work-dir", type=Path, help="Parent for a new, unique benchmark directory")
    args = parser.parse_args()
    if not (0 < args.timeout <= 60):
        parser.error("--timeout must be greater than zero and at most 60 seconds")
    executable = args.executable.expanduser().resolve()
    if not executable.is_file():
        parser.error(f"Executable does not exist: {executable}")
    output = args.output.expanduser().resolve()
    brief = (args.brief or output.with_suffix(".md")).expanduser().resolve()
    if output == brief:
        parser.error("--output and --brief must be different paths")
    parent = args.work_dir.expanduser().resolve() if args.work_dir else None
    if parent:
        parent.mkdir(parents=True, exist_ok=True)
    workspace = Path(tempfile.mkdtemp(prefix="mergedesk-benchmark-", dir=parent))
    report: dict[str, Any] = {
        "schemaVersion": 1, "suiteVersion": SUITE_VERSION, "recordedUtc": utc_now(),
        "status": "running", "timeoutSeconds": args.timeout,
        "sampleCountPerWorkload": 1, "environment": environment_details(executable),
        "executable": {"path": str(executable), "sha256": sha256_file(executable),
                       "sizeBytes": executable.stat().st_size},
        "workspace": str(workspace), "inputsRetained": args.keep_inputs,
        "measurementScope": "startup + comparison + shutdown; excludes fixture generation and hashing",
        "limitations": ["Single smoke sample; not a performance guarantee or product comparison",
                        "Synthetic fixtures; caches and concurrent system workload are uncontrolled",
                        "RSS is child process high-water, not summed process-tree memory",
                        "Offscreen Qt omits interactive display/compositor costs"],
        "workloads": [],
    }
    try:
        version = run_process([str(executable), "--version"],
                              isolated_environment(workspace / "version-env"),
                              workspace / "version-log", min(args.timeout, 10))
        report["executable"]["versionProbe"] = version
        report["executable"]["reportedVersion"] = version.get("stdout", "").strip()
        for make in (make_text, make_folders, make_table, make_image):
            name = make.__name__.removeprefix("make_")
            input_root = workspace / "inputs" / name
            print(f"Preparing {name} fixture...", flush=True)
            try:
                workload = make(input_root)
            except (OSError, MemoryError) as error:
                workload = {"name": name, "title": name, "status": "failed",
                            "reason": f"Could not prepare fixture: {type(error).__name__}: {error}"}
            if "status" in workload:
                report["workloads"].append(workload)
                write_results(output, brief, report)
                print(f"{name}: {workload['status']}: {workload.get('reason', '')}", flush=True)
                continue
            paths = workload.pop("paths")
            arguments = workload.pop("arguments")
            before = input_manifest(input_root)
            workload["inputs"] = manifest_summary(before)
            workload["inputPaths"] = [str(path) for path in paths]
            print(f"Running {name} (timeout {args.timeout:g}s)...", flush=True)
            run = run_process([str(executable), *arguments, *(str(path) for path in paths)],
                              isolated_environment(workspace / "settings" / name),
                              workspace / "logs" / name, args.timeout)
            workload.update(run)
            after = input_manifest(input_root)
            workload["inputsUnchanged"] = before == after
            workload["inputsAfter"] = manifest_summary(after)
            failures = []
            if run.get("launchError"):
                failures.append(run["launchError"])
            if run["timedOut"]:
                failures.append(f"Exceeded {args.timeout:g}-second process limit")
            if run["exitCode"] != workload["expectedExitCode"]:
                failures.append(f"Expected exit {workload['expectedExitCode']}, got {run['exitCode']}")
            if "expectedDiffCount" in workload:
                counts = re.findall(r"^diffs:\s*(\d+)\s*$", run.get("stdout", ""), re.MULTILINE)
                workload["observedDiffCount"] = int(counts[0]) if len(counts) == 1 else None
                if workload["observedDiffCount"] != workload["expectedDiffCount"]:
                    failures.append(f"Expected {workload['expectedDiffCount']} difference blocks, "
                                    f"got {workload['observedDiffCount']}")
            if not workload["inputsUnchanged"]:
                changed = mutation_paths(before, after)
                workload["mutatedPathCount"] = len(changed)
                workload["mutatedPaths"] = changed[:32]
                failures.append("Input files or directory structure changed")
            workload["failures"] = failures
            workload["status"] = "timed_out" if run["timedOut"] else ("failed" if failures else "passed")
            report["workloads"].append(workload)
            write_results(output, brief, report)
            print(f"{name}: {workload['status']}; {run['elapsedSeconds']:.3f}s; "
                  f"exit {run['exitCode']}; inputs unchanged={workload['inputsUnchanged']}", flush=True)
        report["status"] = "passed" if all(item["status"] in ("passed", "skipped")
                                           for item in report["workloads"]) else "failed"
        if version.get("exitCode") != 0 or not report["executable"]["reportedVersion"]:
            report["status"] = "failed"
            report["versionProbeError"] = "Executable version could not be verified."
        report["finishedUtc"] = utc_now()
        # Detect a relink during the run; results must identify one exact executable.
        report["executable"]["unchangedDuringRun"] = sha256_file(executable) == report["executable"]["sha256"]
        if not report["executable"]["unchangedDuringRun"]:
            report["status"] = "failed"
            report["executableChangedError"] = "Executable changed during the run; rerun after the build finishes."
        write_results(output, brief, report)
        print(f"Wrote {output}\nWrote {brief}", flush=True)
        return 0 if report["status"] == "passed" else 1
    except KeyboardInterrupt:
        report["status"] = "interrupted"
        report["finishedUtc"] = utc_now()
        write_results(output, brief, report)
        return 130
    finally:
        if args.keep_inputs:
            print(f"Fixtures and logs retained: {workspace}", flush=True)
        else:
            shutil.rmtree(workspace, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
