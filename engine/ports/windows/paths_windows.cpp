// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 davoda1
// UTF-8 engine paths with native Windows filesystem operations via Qt Core.
#include "pch.h"
#include "paths.h"
#include "Environment.h"
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QRegularExpression>

namespace paths {
namespace {
QString q(const String& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
String s(const QString& value) { return value.toUtf8().toStdString(); }
String native(const QString& value) { return s(QDir::toNativeSeparators(value)); }
bool slash(char c) { return c == '/' || c == '\\'; }
}
bool EndsWithSlash(const String& path) { return !path.empty() && slash(path.back()); }
PATH_EXISTENCE DoesPathExist(const String& path, bool (*isArchive)(const String&)) {
    if (path.empty()) return DOES_NOT_EXIST;
    QFileInfo info(q(env::ExpandEnvironmentVariables(path)));
    if (isArchive && isArchive(path)) return IS_EXISTING_DIR;
    if (!info.exists()) return DOES_NOT_EXIST;
    return info.isDir() ? IS_EXISTING_DIR : IS_EXISTING_FILE;
}
String FindFileName(const String& path) {
    size_t end = path.size();
    while (end && slash(path[end-1])) --end;
    const size_t pos = path.find_last_of("\\/", end ? end-1 : 0);
    return pos == String::npos ? path : path.substr(pos+1);
}
String FindExtension(const String& path) {
    const String name = FindFileName(path);
    const auto dot = name.find_last_of('.');
    return dot == String::npos ? String() : name.substr(dot);
}
String RemoveExtension(const String& path) { return path.substr(0, path.size()-FindExtension(path).size()); }
String GetLongPath(const String& path, bool expand) {
    if (path.empty()) return {};
    const QString input = q(expand ? env::ExpandEnvironmentVariables(path) : path);
    QString absolute = QDir::cleanPath(QFileInfo(input).absoluteFilePath());
    // Qt uppercases drive letters; WinMerge normalization retains the input's
    // spelling while resolving separators, dot components and relative paths.
    if (input.size() >= 2 && input[1] == QLatin1Char(':') &&
        absolute.startsWith(input.left(2), Qt::CaseInsensitive))
        absolute[0] = input[0];
    return native(absolute);
}
void normalize(String& path) {
    path = GetLongPath(path);
    while (path.size() > 3 && EndsWithSlash(path)) path.pop_back();
}
bool CreateIfNeeded(const String& path) { return !path.empty() && QDir().mkpath(q(path)); }
PATH_EXISTENCE GetPairComparability(const PathContext& paths, bool (*isArchive)(const String&)) {
    if (paths.GetSize() < 2) return DOES_NOT_EXIST;
    auto type = DoesPathExist(paths[0], isArchive);
    for (int i=1; i<paths.GetSize(); ++i) {
        if (type != DoesPathExist(paths[i], isArchive)) {
            type = DoesPathExist(paths[0]);
            for (int j=1; j<paths.GetSize(); ++j)
                if (type != DoesPathExist(paths[j])) return DOES_NOT_EXIST;
            return type;
        }
    }
    return type;
}
bool IsDirectory(const String& path) { return QFileInfo(q(path)).isDir(); }
bool IsShortcut(const String& path) { return q(path).endsWith(".lnk", Qt::CaseInsensitive); }
String ExpandShortcut(const String& path) { return native(QFileInfo(q(path)).symLinkTarget()); }
String ConcatPath(const String& path, const String& child) {
    if (path.empty()) return child;
    if (child.empty()) return path;
    if (EndsWithSlash(path)) return path + child.substr(slash(child.front()) ? 1 : 0);
    return path + (slash(child.front()) ? "" : "\\") + child;
}
String GetParentPath(const String& path) {
    String result = path;
    while (result.size() > 3 && EndsWithSlash(result)) result.pop_back();
    const auto pos = result.find_last_of("\\/");
    if (pos != String::npos) result.resize(pos == 2 && result[1] == ':' ? 3 : pos);
    return result;
}
String GetLastSubdir(const String& path) {
    String result = path;
    if (EndsWithSlash(result)) result.pop_back();
    const auto pos = result.find_last_of("\\/");
    if (pos != String::npos && pos > 0) result.erase(0, pos);
    return result;
}
bool IsPathAbsolute(const String& path) {
    return (path.size() >= 3 && path[1] == ':' && slash(path[2])) ||
           (path.size() >= 2 && slash(path[0]) && slash(path[1]));
}
String EnsurePathExist(const String& path) { return CreateIfNeeded(path) ? path : String(); }
void SplitFilename(const String& path, String* parent, String* name, String* ext) {
    const auto slashPos = path.find_last_of("\\/:");
    const String filename = slashPos == String::npos ? path : path.substr(slashPos+1);
    const auto dotPos = filename.find_last_of('.');
    if (parent) *parent = slashPos == String::npos ? String() : path.substr(0, slashPos + (path[slashPos] == ':' ? 1 : 0));
    if (name) *name = ext && dotPos != String::npos ? filename.substr(0, dotPos) : filename;
    if (ext) *ext = dotPos == String::npos ? String() : filename.substr(dotPos+1);
}
String GetPathOnly(const String& path) {
    auto pos = path.find_last_of("\\/");
    return pos == String::npos ? String() : path.substr(0, pos);
}
bool IsURL(const String& path) {
    const QUrl url(q(path));
    return url.isValid() && !url.scheme().isEmpty() && url.scheme().size() > 1;
}
bool IsURLorCLSID(const String& path) { return IsURL(path) || path.rfind("::{", 0) == 0; }
bool isFileURL(const String& path) { return q(path).startsWith("file:", Qt::CaseInsensitive); }
String FromURL(const String& url) { return native(QUrl(q(url)).toLocalFile()); }
String urlEncodeFileName(const String& filename) {
    String encoded = filename;
    strutils::replace(encoded, "%", "%25");
    strutils::replace(encoded, "#", "%23");
    return encoded;
}
bool IsDecendant(const String& path, const String& ancestor) {
    QString child = q(GetLongPath(path));
    QString parent = q(AddTrailingSlash(GetLongPath(ancestor)));
    return child.startsWith(parent, Qt::CaseInsensitive);
}
String ToWindowsPath(const String& path) { String out(path); std::replace(out.begin(),out.end(),'/','\\'); return out; }
String ToUnixPath(const String& path) { String out(path); std::replace(out.begin(),out.end(),'\\','/'); return out; }
bool IsValidName(const String& name) {
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') return false;
    if (name.find_first_of("<>:\"/\\|?*") != String::npos) return false;
    for (unsigned char c : name) if (c < 32) return false;
    return !QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$", QRegularExpression::CaseInsensitiveOption).match(q(name)).hasMatch();
}
bool IsNullDeviceName(const String& name) {
    return q(name).compare("NUL", Qt::CaseInsensitive) == 0 || q(name).compare("\\\\.\\NUL", Qt::CaseInsensitive) == 0;
}
} // namespace paths
