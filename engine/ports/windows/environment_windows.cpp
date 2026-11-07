// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 davoda1
#include "pch.h"
#include "Environment.h"
#include "paths.h"
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QProcessEnvironment>
#include <QRegularExpression>

namespace env {
namespace {
String tempPath, progPath;
QString q(const String& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
String s(const QString& value) { return value.toUtf8().toStdString(); }
}
void SetTemporaryPath(const String& path) { tempPath = paths::AddTrailingSlash(paths::GetLongPath(path)); paths::CreateIfNeeded(tempPath); }
String GetTemporaryPath() { if (tempPath.empty()) tempPath = GetSystemTempPath(); return tempPath; }
String GetTemporaryFileName(const String& path, const String& prefix, int* error) {
    paths::CreateIfNeeded(path);
    QTemporaryFile file(q(paths::ConcatPath(path, prefix + "XXXXXX")));
    file.setAutoRemove(false);
    if (!file.open()) { if (error) *error = EACCES; return {}; }
    if (error) *error = 0;
    return s(QDir::toNativeSeparators(file.fileName()));
}
String GetTempChildPath() {
    QTemporaryDir dir(q(paths::ConcatPath(GetTemporaryPath(), "mergedesk-XXXXXX")));
    if (!dir.isValid()) return {};
    dir.setAutoRemove(false);
    return s(QDir::toNativeSeparators(dir.path()));
}
void SetProgPath(const String& path) { progPath = paths::AddTrailingSlash(path); }
String GetProgPath() {
    if (!progPath.empty()) return progPath;
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (size == 0 || size == path.size()) return {};
    progPath = paths::AddTrailingSlash(paths::GetPathOnly(s(QString::fromWCharArray(path.data(), int(size)))));
    return progPath;
}
String GetWindowsDirectory() { return s(QDir::toNativeSeparators(qEnvironmentVariable("WINDIR"))); }
String GetMyDocuments() { return s(QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))); }
String GetAppDataPath() { return s(QDir::toNativeSeparators(qEnvironmentVariable("APPDATA"))); }
String GetSystemTempPath() { return paths::AddTrailingSlash(s(QDir::toNativeSeparators(QDir::tempPath()))); }
String GetPerInstanceString(const String& name) { return name + std::to_string(GetCurrentProcessId()); }
String ExpandEnvironmentVariables(const String& text) {
    const std::wstring input = q(text).toStdWString();
    DWORD length = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (!length) return text;
    std::vector<wchar_t> output(length);
    if (!ExpandEnvironmentStringsW(input.c_str(), output.data(), length)) return text;
    return s(QString::fromWCharArray(output.data()));
}
bool LoadRegistryFromFile(const String&) { return false; }
bool SaveRegistryToFile(const String&, const String&) { return false; }
} // namespace env
