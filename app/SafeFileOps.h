// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace md {
// Write with an atomic replacement. No direct-write fallback is allowed.
bool writeBytesAtomic(const QString &path, const QByteArray &bytes, QString *error);
bool copyFileAtomic(const QString &source, const QString &target, bool backup, QString *error);
bool createBackup(const QString &path, QString *error);
bool safeDestination(const QString &path, QString *error);
// WinMerge UnicodeSet values: 0=codepage, 1/2=UTF-16, 3=UTF-8, 4/5=UTF-32.
// Conversion refuses characters the original encoding cannot represent.
bool writeTextAtomic(const QString &path, const QStringList &lines,
    const QStringList &endings, bool finalEol, int unicodeSet, int codepage,
    bool bom, QString *error);
}
