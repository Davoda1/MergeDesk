#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-07.
# Run the app's headless selftest suite against a given binary.
# Usage: run_selftests.sh <mergedesk-binary>
# Fixtures are regenerated before every test (selftest-save mutates them).
# GNU coreutils supplies timeout (or gtimeout on systems with prefixed tools).
set -euo pipefail
BIN="${1:?usage: run_selftests.sh <mergedesk-binary>}"
if [[ "$BIN" != /* ]]; then
  binary_directory="$(cd -- "$(dirname -- "$BIN")" && pwd)"
  BIN="$binary_directory/$(basename -- "$BIN")"
fi
if [[ ! -x "$BIN" ]]; then
  printf 'Not an executable: %s\n' "$BIN" >&2
  exit 2
fi
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
if command -v timeout >/dev/null 2>&1; then
  timeout_command=timeout
elif command -v gtimeout >/dev/null 2>&1; then
  timeout_command=gtimeout
else
  printf 'GNU coreutils timeout is required to bound each selftest.\n' >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export XDG_CONFIG_HOME="$TMP/config"
export XDG_CACHE_HOME="$TMP/cache"
export XDG_DATA_HOME="$TMP/data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
# Native trash APIs and any inherited relative fixtures stay disposable too.
cd "$TMP" || exit 2

make_fixtures() {
  awk 'BEGIN{for(i=1;i<=800;i++) print "line " i " common text here"}' \
    > "$TMP/left.txt"
  awk 'BEGIN{for(i=1;i<=800;i++){
    if(i%100==0) print "line " i " CHANGED on right";
    else if(i==250){print "line 250 common text here"; print "extra right line"}
    else print "line " i " common text here"}}' > "$TMP/right.txt"
  printf 'id,name,price\n1,apple,10\n2,banana,20\n3,cherry,30\n4,date,40\n' \
    > "$TMP/left.csv"
  printf 'id,name,price\n1,apple,11\n2,banana,20\n3,cereja,30\n5,elder,50\n' \
    > "$TMP/right.csv"
  # 3-way: one line, three versions (pane-relative merge flow)
  printf 'alpha\nversion = "A"\ncommon\n' > "$TMP/w3a.txt"
  printf 'alpha\nversion = "B"\ncommon\n' > "$TMP/w3b.txt"
  printf 'alpha\nversion = "C"\ncommon\n' > "$TMP/w3c.txt"
  # 64x48 white PNG vs the same with a red and a green box (2 differences)
  base64 -d > "$TMP/left.png" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAEAAAAAwCAYAAAChS3wfAAAAdElEQVR4nO3QQREAAAiAMPuX1hh7yBJwzD43OkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAK0BOkBrgA7QGqADtAboAO0AM+HSwnsOK34AAAAASUVORK5CYII=
PNG
  base64 -d > "$TMP/right.png" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAEAAAAAwCAYAAAChS3wfAAAAiElEQVR4nO3QQQrDMBAEwX16fm6fc7INMkWibtBNLEPNsXmjB+gC0AN0AegBugD0AF0AeoDuPsDMs/cjBXD/ZwABBBBAAAEEEEAAmwL8aQHoAboA9ABdAHqALgA94Kr5zNdbfn/5xcUFEMDmAG8XgB6gC0AP0AWgB+gC0AN0AegBugD0AN32ACcCDIwDSDcRXgAAAABJRU5ErkJggg==
PNG
  # 40x40 with a blue box past the edge of a 30x30 one: copying it all
  # grows the smaller image (WinIMerge's #42)
  base64 -d > "$TMP/big.png" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAACgAAAAoCAIAAAADnC86AAAANUlEQVR4nO3NIREAAAgAMZLQPwqxaIDAINjduxeLyjwpwGAwGAwGg8FgMBj8Ep4/GAwGr+EGjRBZW+HdLAIAAAAASUVORK5CYII=
PNG
  base64 -d > "$TMP/small.png" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAB4AAAAeCAIAAAC0Ujn1AAAAJ0lEQVR4nGM4ISdHI8QwavSo0aNGjxo9avSo0aNGjxo9avSo0eQjAE1lkj7s7aamAAAAAElFTkSuQmCC
PNG
}

fails=0
run_one() { # $1 = selftest name, $2/$3[/$4] = files
  make_fixtures
  if "$timeout_command" --kill-after=5s 180s "$BIN" "--$1" "${@:2}" > "$TMP/output.txt" 2>&1; then
    echo "ok: $1"
  else
    echo "FAIL: $1 (exit $?)"
    # Platform/plugin errors from Qt can explain a real application failure.
    tail -n 40 "$TMP/output.txt" | sed 's/^/    /'
    fails=$((fails + 1))
  fi
}

for t in selftest-merge selftest-save selftest-undo selftest-undo-scroll \
         selftest-nav selftest-copy; do
  run_one "$t" "$TMP/left.txt" "$TMP/right.txt"
done
run_one selftest-undo-rescan
run_one selftest-undo-ghosts
run_one selftest-last-line
run_one selftest-scroll-sync
run_one selftest-diff-from-cursor
run_one selftest-diff-scroll
run_one selftest-table-options
run_one selftest-image-options
run_one selftest-table-3way
run_one selftest-table-edit
run_one selftest-table-ignore-column
run_one selftest-tree-sitter
run_one selftest-editor-tabs
run_one selftest-eol
run_one selftest-command-line
run_one selftest-subfolders
run_one selftest-codepage
run_one selftest-folder-selection
run_one selftest-hex
run_one selftest-hex-edit
run_one selftest-hex-header
run_one selftest-path-bars
run_one selftest-unique-items
run_one selftest-location-pane
run_one selftest-find-replace
run_one selftest-markers
run_one selftest-open-enter
run_one selftest-archive
run_one selftest-archive-mixed
run_one selftest-folder3
run_one selftest-service
run_one selftest-marker
run_one selftest-folder-sync
run_one selftest-folder3-ops
run_one selftest-folder-content
run_one selftest-compare-methods
run_one selftest-show-filters
# Native menu check runs under cocoa and skips offscreen. About and Options
# verify the current branding and language choices in every supported language.
for lang in en_US hu de fr; do
  MERGEDESK_LANGUAGE=$lang run_one selftest-app-menu
  MERGEDESK_LANGUAGE=$lang run_one selftest-about
  MERGEDESK_LANGUAGE=$lang run_one selftest-options
done
run_one selftest-general-options
run_one selftest-tab-close
run_one selftest-line-diffs
run_one selftest-copy-granularity
run_one selftest-pane-menu
run_one selftest-table-menu
run_one selftest-ignored-diffs
run_one selftest-syntax-menu
run_one selftest-view-eol
run_one selftest-identical
run_one selftest-reload
run_one selftest-compare-options
run_one selftest-filters
run_one selftest-file-filters
run_one selftest-filter-menu
run_one selftest-display-filter
run_one selftest-line-filter
run_one selftest-theme
run_one selftest-menu-roles
for lang in en_US hu de fr; do
  MERGEDESK_LANGUAGE=$lang run_one selftest-qt-i18n
done
# the accelerator letters of the menus, one item each, in every catalog
# (English keeps WinMerge's letters as they stand)
for lang in hu de fr; do
  MERGEDESK_LANGUAGE=$lang run_one selftest-menu-mnemonics
done
# The application must remain free of promotional prompts in every language.
for lang in en_US hu de fr; do
  MERGEDESK_LANGUAGE=$lang run_one selftest-star-prompt
done
run_one selftest-desktop-integration
if [ "$(uname -s)" = Linux ]; then
  run_one selftest-file-manager-integration
fi
run_one selftest-table "$TMP/left.csv" "$TMP/right.csv"
run_one selftest-image "$TMP/left.png" "$TMP/right.png"
run_one selftest-image "$TMP/big.png" "$TMP/small.png"
run_one selftest-merge3 "$TMP/w3a.txt" "$TMP/w3b.txt" "$TMP/w3c.txt"

echo "== $fails failure(s)"
exit "$((fails > 0 ? 1 : 0))"
