// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 davoda1
#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <iterator>
#include <QString>
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#endif

namespace mergedesk {
// Call before constructing QApplication. The real ELF/PE executable can live
// at the portable root or in /usr/bin; no launcher or global /usr/bin/qt.conf
// is needed. Only complete application-private runtime folders are accepted.
inline QString bundledRuntimeRoot() {
    QString executable;
#ifdef Q_OS_WIN
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    if (length && length < std::size(path)) executable = QString::fromWCharArray(path, int(length));
#elif defined(Q_OS_LINUX)
    char path[32768]{};
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path)-1);
    if (length > 0) executable = QString::fromUtf8(path, int(length));
#endif
    if (executable.isEmpty()) return {};
    const QString directory = QFileInfo(executable).absolutePath();
    const QStringList candidates{directory, QDir(directory).filePath("../lib/mergedesk")};
    for (const auto& candidate : candidates) {
        QDir dir(candidate);
        if (dir.exists("plugins/platforms") && (dir.exists("libexec/QtWebEngineProcess") || dir.exists("QtWebEngineProcess.exe")))
            return dir.absolutePath();
    }
    return {};
}
inline void configureBundledRuntime() {
    const QString root = bundledRuntimeRoot();
    if (root.isEmpty()) return;
    const QDir dir(root);
    QCoreApplication::setLibraryPaths({dir.filePath("plugins")});
    auto setIfEmpty = [&](const char* key, const QString& path) {
        if (qEnvironmentVariableIsEmpty(key)) qputenv(key, QFile::encodeName(path));
    };
#ifdef Q_OS_WIN
    setIfEmpty("QTWEBENGINEPROCESS_PATH", dir.filePath("QtWebEngineProcess.exe"));
#else
    setIfEmpty("QTWEBENGINEPROCESS_PATH", dir.filePath("libexec/QtWebEngineProcess"));
#endif
    setIfEmpty("QTWEBENGINE_RESOURCES_PATH", dir.filePath("resources"));
    setIfEmpty("QTWEBENGINE_LOCALES_PATH", dir.filePath("translations/qtwebengine_locales"));
}
} // namespace mergedesk
