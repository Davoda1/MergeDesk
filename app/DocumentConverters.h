// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <QTemporaryDir>
#include <QString>
#include <QStringList>

namespace lm {
struct DocumentText {
    QString text;
    QString format;
    QStringList warnings;
};
struct ConversionOptions {
    /** Empty means discover the executable on PATH. No shell is used. */
    QString pdfToTextExecutable;
    QString officeExecutable;
    int timeoutMilliseconds = 90000;
    qint64 maximumTextBytes = 64 * 1024 * 1024;
    bool allowExternalTools = true;
};
struct ConvertedDocuments {
    QStringList paths;
    QStringList labels;
    QStringList warnings;
    /** Keep this alive as long as a comparison uses the temporary paths. */
    std::shared_ptr<QTemporaryDir> temporaryDirectory;
};

bool isDocumentPath(const QString &path);
QStringList supportedDocumentExtensions();
/** Text/values extraction, not visual rendering. Never writes to the source.
 * Native: DOCX/XLSX/PPTX/ODT/ODS/ODP. PDF requires pdftotext; old binary Office
 * requires LibreOffice. Rejects encrypted, malformed or over-budget inputs. */
bool convertDocument(const QString &path, DocumentText *result, QString *error,
                     const ConversionOptions &options = {});
/** Batch helper for a read-only comparison. Ordinary text inputs are passed
 * through; document inputs become UTF-8 files. All conversions must succeed. */
bool convertDocuments(const QStringList &paths, ConvertedDocuments *result, QString *error,
                      const ConversionOptions &options = {});
} // namespace lm
