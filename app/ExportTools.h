// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace lm {
struct FolderCompareResult;

/** Two input files; optional UTF-8 bytes are current editor contents, including
 * their actual final-newline state. Exactly two snapshots must be supplied,
 * or none to compare files on disk. Names are shown in the patch/report. */
struct TextExportInput {
    QStringList paths;
    QStringList labels;
    QList<QByteArray> utf8Snapshots;
};

/** Exact unified patch from the vendored WinMerge engine. Ignore rules and
 * plugin transformations are deliberately disabled so the patch is applicable.
 * Atomic destination write; refuses binary input and input/output collisions. */
bool exportUnifiedPatch(const TextExportInput &input, const QString &destination,
                        QString *error, int contextLines = 3);
/** Self-contained HTML, with escaped text and light/dark CSS. Uses the exact
 * engine patch, preserving final-newline diagnostics. */
bool exportHtmlReport(const TextExportInput &input, const QString &destination,
                      QString *error, int contextLines = 3);
/** Export the completed folder comparison supplied by the caller. */
bool exportFolderHtmlReport(const FolderCompareResult &result, const QStringList &roots,
                            const QString &destination, QString *error);
bool exportFolderCsvReport(const FolderCompareResult &result, const QStringList &roots,
                           const QString &destination, QString *error);
} // namespace lm
