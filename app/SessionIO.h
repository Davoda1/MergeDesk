// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "CommandLine.h"
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace lm {

/** A WinMerge-compatible XML project. Each <paths> element is one tab.
 * Project files describe paths and options, never unsaved document contents.
 * Unsupported fields are retained for round trips and reported in warnings. */
struct ProjectSession {
    QList<CommandLine> comparisons;
    QList<QMap<QString, QString>> extraFields;
    QStringList warnings;
};

bool loadProject(const QString &path, ProjectSession *session, QString *error);
bool saveProject(const QString &path, const ProjectSession &session, QString *error,
                 bool relativePaths = true);
/** Portable INI export/import of this application's QSettings. Import merges
 * supplied keys only. Restart after import to reload cached engine settings. */
bool exportSettings(const QString &path, QString *error);
/** Also reads WinMerge INI and exported .reg files as data, maps supported
 * preferences and reports skipped keys. Executable converter paths are not
 * imported. Never executes Registry Editor. */
bool importSettings(const QString &path, QString *error, QStringList *warnings = nullptr);

} // namespace lm
