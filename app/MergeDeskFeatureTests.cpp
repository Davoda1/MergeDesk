// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "MergeDeskFeatureTests.h"
#include "SessionIO.h"
#include "ExportTools.h"
#include "DocumentConverters.h"
#include "FolderCompareDriver.h"
#include "diff.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <archive.h>
#include <archive_entry.h>

namespace lm {
namespace {
struct Checks {
    int passed = 0, failed = 0;
    void expect(bool condition, const QString &name, const QString &detail = {})
    {
        if (condition) { ++passed; return; }
        ++failed;
        QTextStream(stderr) << "FAIL: " << name << (detail.isEmpty() ? QString() : QStringLiteral(": ") + detail) << '\n';
    }
};

bool put(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray utf16le(const QString &text, bool bom = true)
{
    QByteArray bytes = bom ? QByteArray::fromHex("fffe") : QByteArray();
    for (const QChar c : text) { bytes += char(c.unicode() & 255); bytes += char(c.unicode() >> 8); }
    return bytes;
}

QByteArray get(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

la_ssize_t writeArchive(struct archive *, void *opaque, const void *bytes, size_t count)
{
    return static_cast<QFile *>(opaque)->write(static_cast<const char *>(bytes), static_cast<qint64>(count));
}

bool zip(const QString &path, const QMap<QString, QByteArray> &parts)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    if (!writer || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK
        || archive_write_open(writer.get(), &file, nullptr, writeArchive, nullptr) != ARCHIVE_OK) return false;
    for (auto it = parts.cbegin(); it != parts.cend(); ++it) {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname_utf8(entry.get(), it.key().toUtf8().constData());
        archive_entry_set_size(entry.get(), it.value().size());
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0600);
        if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK
            || archive_write_data(writer.get(), it.value().constData(), it.value().size()) != it.value().size()) return false;
    }
    return archive_write_close(writer.get()) == ARCHIVE_OK;
}

QList<QByteArray> physicalLines(const QByteArray &bytes)
{
    QList<QByteArray> lines;
    qsizetype start = 0;
    while (start < bytes.size()) {
        const qsizetype end = bytes.indexOf('\n', start);
        if (end < 0) { lines.append(bytes.mid(start)); break; }
        lines.append(bytes.mid(start, end - start + 1)); start = end + 1;
    }
    return lines;
}

// An independent, strict unified-patch reader verifies that the engine's
// output reconstructs the target, including CRLF and missing final newlines.
// It intentionally does not use the application's comparison implementation.
bool applyPatch(const QByteArray &old, const QByteArray &patch, QByteArray *result)
{
    if (patch.isEmpty()) { *result = old; return true; }
    const auto source = physicalLines(old);
    const auto lines = patch.split('\n');
    QByteArray output;
    int oldPosition = 0, newPosition = 0;
    bool sawHunk = false;
    const QRegularExpression header(QStringLiteral("^@@ -([0-9]+)(?:,([0-9]+))? \\+([0-9]+)(?:,([0-9]+))? @@"));
    for (int i = 0; i < lines.size(); ++i) {
        if (!lines.at(i).startsWith("@@ ")) continue;
        const auto match = header.match(QString::fromLatin1(lines.at(i)));
        if (!match.hasMatch()) return false;
        sawHunk = true;
        const int oldCount = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
        const int newCount = match.captured(4).isEmpty() ? 1 : match.captured(4).toInt();
        const int oldStart = match.captured(1).toInt() - (oldCount == 0 ? 0 : 1);
        const int newStart = match.captured(3).toInt() - (newCount == 0 ? 0 : 1);
        if (oldStart < oldPosition || oldStart > source.size()) return false;
        while (oldPosition < oldStart) { output += source.at(oldPosition++); ++newPosition; }
        if (newPosition != newStart) return false;
        int oldRead = 0, newRead = 0;
        while (i + 1 < lines.size() && !lines.at(i + 1).startsWith("@@ ") && (oldRead < oldCount || newRead < newCount)) {
            const QByteArray line = lines.at(++i);
            if (line.isEmpty()) return false;
            const char kind = line.at(0);
            if (kind != ' ' && kind != '+' && kind != '-') return false;
            QByteArray payload = line.mid(1) + '\n';
            if (i + 1 < lines.size() && lines.at(i + 1) == "\\ No newline at end of file") { payload.chop(1); ++i; }
            if (kind != '+') {
                if (oldPosition >= source.size() || source.at(oldPosition) != payload) return false;
                ++oldPosition; ++oldRead;
            }
            if (kind != '-') { output += payload; ++newPosition; ++newRead; }
        }
        if (oldRead != oldCount || newRead != newCount) return false;
    }
    if (!sawHunk) return false;
    while (oldPosition < source.size()) output += source.at(oldPosition++);
    *result = std::move(output); return true;
}

void projectTests(const QTemporaryDir &dir, Checks &check)
{
    ProjectSession project;
    CommandLine first;
    first.paths = {dir.filePath(QStringLiteral("left & one.txt")), dir.filePath(QStringLiteral("middle.txt")), dir.filePath(QStringLiteral("right.txt"))};
    first.leftDesc = QStringLiteral("Left <working>"); first.rightDesc = QStringLiteral("Right — ü");
    first.leftFlags = CommandLine::ReadOnly; first.middleFlags = CommandLine::ReadOnly;
    first.windowType = CommandLine::WindowType::Table; first.recursive = false;
    first.tableDelimiter = QLatin1Char('\t'); first.tableQuote = QLatin1Char('"'); first.tableAllowNewlinesInQuotes = true;
    first.compareMethod = 1; first.fileFilter = QStringLiteral("*.cpp;*.h");
    first.options = {{QStringLiteral("Settings/IgnoreCase"), QStringLiteral("1")}, {QStringLiteral("Settings/IgnoreSpace"), QStringLiteral("2")}};
    first.outputPath = dir.filePath(QStringLiteral("merged.txt"));
    CommandLine second;
    second.paths = {QStringLiteral("https://example.invalid/a?x=1&y=2"), QStringLiteral("https://example.invalid/b")};
    second.windowType = CommandLine::WindowType::WebPage;
    project.comparisons = {first, second};
    project.extraFields = {{{QStringLiteral("hidden-list"), QStringLiteral("<hidden-list><hidden-item>keep &amp; preserve.txt</hidden-item></hidden-list>")}}, {}};
    const QString path = dir.filePath(QStringLiteral("roundtrip.WinMerge"));
    QString error;
    check.expect(saveProject(path, project, &error), QStringLiteral("save project"), error);
    const auto raw = get(path);
    check.expect(!raw.contains(dir.path().toUtf8()), QStringLiteral("project saves relative filesystem paths"));
    ProjectSession loaded;
    check.expect(loadProject(path, &loaded, &error), QStringLiteral("load saved project"), error);
    check.expect(loaded.comparisons.size() == 2, QStringLiteral("multiple project comparisons"));
    if (loaded.comparisons.size() == 2) {
        const auto &actual = loaded.comparisons.first();
        check.expect(actual.paths == first.paths && actual.leftDesc == first.leftDesc && actual.rightDesc == first.rightDesc, QStringLiteral("project paths and descriptions round trip"));
        check.expect(actual.leftFlags == first.leftFlags && actual.middleFlags == first.middleFlags && actual.recursive == first.recursive, QStringLiteral("project read-only and recursion round trip"));
        check.expect(actual.tableDelimiter == first.tableDelimiter && actual.tableQuote == first.tableQuote && actual.tableAllowNewlinesInQuotes == first.tableAllowNewlinesInQuotes, QStringLiteral("table project options round trip"));
        check.expect(actual.compareMethod == first.compareMethod && actual.fileFilter == first.fileFilter && actual.outputPath == first.outputPath, QStringLiteral("comparison project options round trip"));
        check.expect(loaded.comparisons[1].paths == second.paths, QStringLiteral("project URLs are not resolved as files"));
        check.expect(loaded.extraFields.first().value(QStringLiteral("hidden-list")).contains(QStringLiteral("hidden-item")) && !loaded.warnings.isEmpty(), QStringLiteral("unsupported project XML is retained and reported"));
    }
    ProjectSession noQuote = project;
    noQuote.comparisons[0].tableQuote = QChar();
    const QString noQuotePath = dir.filePath(QStringLiteral("no-quote.WinMerge"));
    ProjectSession noQuoteLoaded;
    check.expect(saveProject(noQuotePath, noQuote, &error) && loadProject(noQuotePath, &noQuoteLoaded, &error)
        && noQuoteLoaded.comparisons.first().tableQuote && noQuoteLoaded.comparisons.first().tableQuote->isNull(), QStringLiteral("disabled table quoting round trips as empty XML text"), error);
    const QString bad = dir.filePath(QStringLiteral("bad.WinMerge"));
    put(bad, "<!DOCTYPE project [<!ENTITY leak SYSTEM 'file:///etc/passwd'>]><project><paths><left>&leak;</left><right>x</right></paths></project>");
    check.expect(!loadProject(bad, &loaded, &error) && !error.isEmpty(), QStringLiteral("reject project DTD"));
    put(bad, "<project><paths><left>a</left><left>b</left><right>c</right></paths></project>");
    check.expect(!loadProject(bad, &loaded, &error), QStringLiteral("reject duplicate project paths"));
    put(bad, "<project><paths><left>a</left><right>b</right><left-readonly>maybe</left-readonly></paths></project>");
    check.expect(!loadProject(bad, &loaded, &error), QStringLiteral("reject malformed project boolean"));
    put(bad, "<project><paths><left>a</left><right>b</right><compare-method>999</compare-method></paths></project>");
    check.expect(!loadProject(bad, &loaded, &error), QStringLiteral("reject invalid compare method"));
    put(bad, "<project><paths><left>a</left><right>b</right><unpacker>Legacy.dll</unpacker></paths></project>");
    check.expect(loadProject(bad, &loaded, &error) && loaded.warnings.join(QLatin1Char(' ')).contains(QStringLiteral("unpacker")), QStringLiteral("legacy plugin field is reported, not executed"));
    check.expect(saveProject(path, loaded, &error) && get(path).contains("Legacy.dll"), QStringLiteral("legacy plugin XML survives re-save"));
    put(bad, "<project><paths><left>a</left><right>b</right></paths></project><other/>");
    check.expect(!loadProject(bad, &loaded, &error), QStringLiteral("reject trailing XML roots"));
}

void settingsTests(const QTemporaryDir &dir, Checks &check)
{
    const QString organization = QCoreApplication::organizationName();
    const QString application = QCoreApplication::applicationName();
    const auto format = QSettings::defaultFormat();
    QCoreApplication::setOrganizationName(QStringLiteral("MergeDeskFeatureTests-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCoreApplication::setApplicationName(QStringLiteral("TemporarySettings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QString settingsPath;
    {
        QSettings settings; settingsPath = settings.fileName();
        settings.setValue(QStringLiteral("Settings/IgnoreCase"), true);
        settings.setValue(QStringLiteral("UI/LastPaths"), QStringList{QStringLiteral("a & b"), QStringLiteral("ünicode")});
        settings.setValue(QStringLiteral("UI/Geometry"), QByteArray("\0\1\2", 3)); settings.sync();
    }
    QString error;
    const QString ini = dir.filePath(QStringLiteral("export.ini"));
    check.expect(exportSettings(ini, &error), QStringLiteral("export native INI settings"), error);
    { QSettings s; s.setValue(QStringLiteral("Settings/IgnoreCase"), false); s.setValue(QStringLiteral("Keep/Unspecified"), 23); s.sync(); }
    check.expect(importSettings(ini, &error), QStringLiteral("import native INI settings"), error);
    {
        QSettings s;
        check.expect(s.value(QStringLiteral("Settings/IgnoreCase")).toBool(), QStringLiteral("settings boolean restored"));
        check.expect(s.value(QStringLiteral("UI/LastPaths")).toStringList() == QStringList{QStringLiteral("a & b"), QStringLiteral("ünicode")}, QStringLiteral("settings list and Unicode round trip"));
        check.expect(s.value(QStringLiteral("UI/Geometry")).toByteArray() == QByteArray("\0\1\2", 3), QStringLiteral("settings byte array round trip"));
        check.expect(s.value(QStringLiteral("Keep/Unspecified")).toInt() == 23, QStringLiteral("settings import preserves unspecified values"));
        s.clear(); s.sync();
    }
    const QString untrustedNative=dir.filePath(QStringLiteral("untrusted-native.ini"));
    {
        QSettings s;
        s.setValue(QStringLiteral("Converters/PdfToText"),QStringLiteral("trusted-pdf-converter"));
        s.sync();
    }
    check.expect(put(untrustedNative,
        "[MergeDeskExport]\nFormatVersion=1\n[Converters]\nPdfToText=untrusted-program\nLibreOffice=untrusted-program\n"
        "[CONVERTERS]\nPDFTOTEXT=untrusted-program\n[Settings]\nIgnoreCase=true\n"),
        QStringLiteral("create native settings with executable paths"));
    QStringList executableWarnings;
    check.expect(importSettings(untrustedNative,&error,&executableWarnings),QStringLiteral("import ordinary settings while rejecting executable paths"),error);
    {
        QSettings s;
        check.expect(s.value(QStringLiteral("Converters/PdfToText")).toString()==QStringLiteral("trusted-pdf-converter")
            &&!s.contains(QStringLiteral("Converters/LibreOffice"))
            &&s.value(QStringLiteral("CONVERTERS/PDFTOTEXT")).toString()!=QStringLiteral("untrusted-program")
            &&s.value(QStringLiteral("Settings/IgnoreCase")).toBool()
            &&executableWarnings.join(QLatin1Char('\n')).contains(QStringLiteral("PdfToText"),Qt::CaseInsensitive)
            &&executableWarnings.join(QLatin1Char('\n')).contains(QStringLiteral("LibreOffice"),Qt::CaseInsensitive),
            QStringLiteral("settings import cannot replace or introduce a converter executable, including case variants"));
        s.clear();s.sync();
    }
    const QString reg = dir.filePath(QStringLiteral("WinMerge.reg"));
    const QString registry = QStringLiteral("Windows Registry Editor Version 5.00\r\n\r\n"
        "[HKEY_CURRENT_USER\\Software\\Thingamahoochie\\WinMerge\\Settings]\r\n"
        "\"IgnoreCase\"=dword:00000001\r\n\"ScrollToFirst\"=dword:00000000\r\n"
        "\"CSVFilePatterns\"=\"*.csv;*.ünicode\"\r\n\"WindowsOnlyThing\"=dword:00000007\r\n"
        "[HKEY_CURRENT_USER\\Software\\OtherApplication]\r\n\"Run\"=\"never executed\"\r\n"
        "[-HKEY_CURRENT_USER\\Software\\Thingamahoochie\\WinMerge]\r\n");
    put(reg, utf16le(registry));
    QStringList warnings;
    check.expect(importSettings(reg, &error, &warnings), QStringLiteral("read WinMerge UTF-16 Registry export as data"), error);
    {
        QSettings s;
        check.expect(s.value(QStringLiteral("Settings/IgnoreCase")).toBool()
            && !s.value(QStringLiteral("General/ScrollToFirstDiff"), true).toBool()
            && s.value(QStringLiteral("TableCompare/CSVFilePatterns")).toString() == QStringLiteral("*.csv;*.ünicode"), QStringLiteral("Registry preferences map to native keys"));
        check.expect(!s.contains(QStringLiteral("Settings/WindowsOnlyThing")) && !s.contains(QStringLiteral("Run"))
            && warnings.join(QLatin1Char('\n')).contains(QStringLiteral("WindowsOnlyThing")), QStringLiteral("unsupported Registry keys reported and omitted"));
        s.setValue(QStringLiteral("Settings/IgnoreCase"), false); s.sync();
    }
    put(reg, "Windows Registry Editor Version 5.00\n[HKEY_CURRENT_USER\\Software\\Thingamahoochie\\WinMerge\\Settings]\n\"IgnoreCase\"=dword:00000001\n\"IgnoreSpace\"=dword:zz\n");
    check.expect(!importSettings(reg, &error, &warnings) && !QSettings().value(QStringLiteral("Settings/IgnoreCase")).toBool(), QStringLiteral("malformed Registry file imports no partial settings"));
    const QString legacyIni = dir.filePath(QStringLiteral("WinMerge.ini"));
    put(legacyIni, "[WinMerge]\nSettings/IgnoreSpace=2\nSettings/UnimplementedThing=1\nSettings/TabSize=999\n");
    check.expect(importSettings(legacyIni, &error, &warnings) && QSettings().value(QStringLiteral("Settings/IgnoreSpace")).toInt() == 2
        && warnings.size() == 2, QStringLiteral("WinMerge INI maps supported keys and reports unknown or invalid values"), error + warnings.join(QLatin1Char(' ')));
    put(legacyIni, utf16le(QStringLiteral("[Settings]\nIgnoreSpace=1\n")));
    check.expect(importSettings(legacyIni, &error, &warnings) && QSettings().value(QStringLiteral("Settings/IgnoreSpace")).toInt() == 1, QStringLiteral("UTF-16 WinMerge INI supported"), error);
    const QByteArray expanded = utf16le(QStringLiteral("*.cpp;*.h") + QChar(0), false);
    QStringList hex;
    for (const char byte : expanded) hex.append(QString::number(static_cast<unsigned char>(byte), 16).rightJustified(2, QLatin1Char('0')));
    put(reg, QByteArray("Windows Registry Editor Version 5.00\n[HKEY_CURRENT_USER\\Software\\Thingamahoochie\\WinMerge\\Settings]\n\"CSVFilePatterns\"=hex(2):") + hex.join(QLatin1Char(',')).toLatin1() + '\n');
    check.expect(importSettings(reg, &error, &warnings) && QSettings().value(QStringLiteral("TableCompare/CSVFilePatterns")).toString() == QStringLiteral("*.cpp;*.h"), QStringLiteral("Registry UTF-16 string data parsed without expansion"), error);
    { QSettings s; s.clear(); s.sync(); }
    check.expect(!importSettings(dir.filePath(QStringLiteral("does-not-exist.ini")), &error), QStringLiteral("missing settings import fails"));
    QFile::remove(settingsPath); QDir().rmdir(QFileInfo(settingsPath).absolutePath());
    QSettings::setDefaultFormat(format);
    QCoreApplication::setOrganizationName(organization); QCoreApplication::setApplicationName(application);
}

void exportTests(const QTemporaryDir &dir, Checks &check)
{
    QString error;
    TextExportInput input;
    input.paths = {dir.filePath(QStringLiteral("original.txt")), dir.filePath(QStringLiteral("changed.txt"))};
    input.utf8Snapshots = {QByteArray("alpha\nbeta"), QByteArray("alpha\nBETA\ngamma\n")};
    const QString patchPath = dir.filePath(QStringLiteral("changes.patch"));
    const int savedIgnore = ::ignore_case_flag, savedContext = ::context, savedStyle = ::output_style;
    ::ignore_case_flag = 1; ::context = 17; ::output_style = OUTPUT_CONTEXT;
    check.expect(exportUnifiedPatch(input, patchPath, &error), QStringLiteral("export exact unified patch"), error);
    check.expect(::ignore_case_flag == 1 && ::context == 17 && ::output_style == OUTPUT_CONTEXT, QStringLiteral("patch restores engine thread-local options"));
    ::ignore_case_flag = savedIgnore; ::context = savedContext; ::output_style = savedStyle;
    const auto patch = get(patchPath);
    check.expect(patch.contains("\\ No newline at end of file"), QStringLiteral("patch has final-newline marker"));
    QByteArray reconstructed;
    check.expect(applyPatch(input.utf8Snapshots.first(), patch, &reconstructed) && reconstructed == input.utf8Snapshots.last(), QStringLiteral("patch reconstructs missing-newline change"));
    check.expect(!exportUnifiedPatch(input, input.paths.first(), &error), QStringLiteral("patch cannot overwrite an input path"));
    put(patchPath, "previous export");
    check.expect(!exportUnifiedPatch(input, patchPath, &error, -1) && get(patchPath) == "previous export", QStringLiteral("failed patch preserves existing destination"));
    input.utf8Snapshots = {QByteArray("same\n"), QByteArray("same\n")};
    check.expect(exportUnifiedPatch(input, patchPath, &error) && get(patchPath).isEmpty(), QStringLiteral("identical input yields empty patch"));
    input.utf8Snapshots = {QByteArray(2000001, '\n'), QByteArray("b\n")};
    check.expect(!exportUnifiedPatch(input, patchPath, &error) && error.contains(QStringLiteral("2,000,000")), QStringLiteral("patch line-count budget enforced before line allocation"));
    input.utf8Snapshots = {QByteArray("a\0b", 3), QByteArray("different")};
    check.expect(!exportUnifiedPatch(input, patchPath, &error), QStringLiteral("binary snapshots rejected"));
    input.utf8Snapshots = {QByteArray("\xff", 1), QByteArray("different")};
    check.expect(!exportUnifiedPatch(input, patchPath, &error), QStringLiteral("malformed UTF-8 snapshots rejected"));
    input.utf8Snapshots.clear(); put(input.paths[0], QByteArray("a\0b", 3)); put(input.paths[1], QByteArray("a\0c", 3));
    check.expect(!exportUnifiedPatch(input, patchPath, &error), QStringLiteral("on-disk binary files rejected"));
    QRandomGenerator random(0x4d657267u);
    bool randomOk = true;
    for (int trial = 0; trial < 96 && randomOk; ++trial) {
        auto document = [&]() {
            QByteArray bytes;
            const int count = random.bounded(15);
            const QByteArray ending = random.bounded(2) ? "\n" : "\r\n";
            for (int line = 0; line < count; ++line) {
                bytes += QByteArray::number(random.bounded(5));
                if (line + 1 < count || random.bounded(2)) bytes += ending;
            }
            return bytes;
        };
        const QByteArray left = document(), right = document();
        input.utf8Snapshots = {left, right};
        randomOk = exportUnifiedPatch(input, patchPath, &error, trial % 5)
            && applyPatch(left, get(patchPath), &reconstructed) && reconstructed == right;
        if (!randomOk) error += QStringLiteral(" trial=%1 left=%2 right=%3 patch=%4").arg(trial).arg(QString::fromLatin1(left.toHex()), QString::fromLatin1(right.toHex()), QString::fromLatin1(get(patchPath).toHex()));
    }
    check.expect(randomOk, QStringLiteral("96 deterministic patch reconstruction cases"), error);
    const QList<QPair<QByteArray, QByteArray>> edgeCases {
        {QByteArray("a\r\nb\r\n"), QByteArray("a\r\nB\r\n")},
        {QByteArray("a\rb\r"), QByteArray("a\rc\r")},
        {QByteArray("\tfoo\\r\nx\r\n"), QByteArray("\tbar\\r\nx\n")},
        {QByteArray::fromHex("efbbbf") + "same\n", QByteArray("same\n")},
        {QStringLiteral("café\n").toUtf8(), QStringLiteral("世界\n").toUtf8()}
    };
    bool edgesOk = true;
    for (const auto &edge : edgeCases) {
        input.utf8Snapshots = {edge.first, edge.second};
        if (!exportUnifiedPatch(input, patchPath, &error) || !applyPatch(edge.first, get(patchPath), &reconstructed) || reconstructed != edge.second) { edgesOk = false; break; }
    }
    check.expect(edgesOk, QStringLiteral("patch preserves CRLF, lone CR, tabs, backslashes, BOM and Unicode"), error);
    const QString patchProgram = QStandardPaths::findExecutable(QStringLiteral("patch"));
    if (!patchProgram.isEmpty()) {
        input.utf8Snapshots = {QByteArray("alpha\r\nbeta"), QByteArray("ALPHA\r\nbeta\r\n")};
        bool ok = exportUnifiedPatch(input, patchPath, &error);
        const QString original = dir.filePath(QStringLiteral("gnu-original.txt")), applied = dir.filePath(QStringLiteral("gnu-applied.txt"));
        ok = ok && put(original, input.utf8Snapshots.first());
        QProcess process;
        process.start(patchProgram, {QStringLiteral("--batch"), QStringLiteral("--binary"), QStringLiteral("--silent"),
            QStringLiteral("--input"), patchPath, QStringLiteral("--output"), applied, original});
        ok = ok && process.waitForStarted(5000) && process.waitForFinished(10000) && process.exitCode() == 0 && get(applied) == input.utf8Snapshots.last();
        check.expect(ok, QStringLiteral("GNU patch applies exported CRLF patch byte-for-byte"), QString::fromLocal8Bit(process.readAllStandardError()));
    }
    input.labels = {QStringLiteral("<script>alert(1)</script>"), QStringLiteral("Right & ü")};
    input.utf8Snapshots = {QByteArray("old\n"), QByteArray("<script>alert(2)</script>\n")};
    const QString html = dir.filePath(QStringLiteral("report.html"));
    check.expect(exportHtmlReport(input, html, &error), QStringLiteral("export HTML report"), error);
    const auto report = get(html);
    check.expect(!report.contains("<script>") && report.contains("&lt;script&gt;") && report.contains("Content-Security-Policy"), QStringLiteral("HTML report escapes document text and labels"));
    check.expect(report.contains("prefers-color-scheme:dark"), QStringLiteral("report supports light and dark display"));
    FolderCompareResult folder; folder.ok = true; folder.sides = 2; folder.different = 1;
    FolderCompareItem item; item.name = QStringLiteral("=SUM(1,2)"); item.category = FolderCompareItem::Different;
    item.size[0] = 10; item.size[1] = 12; folder.items.append(item);
    const QStringList roots {dir.filePath(QStringLiteral("left")), dir.filePath(QStringLiteral("right"))};
    const QString csv = dir.filePath(QStringLiteral("folder.csv"));
    check.expect(exportFolderCsvReport(folder, roots, csv, &error) && get(csv).contains("\"'=SUM(1,2)\""), QStringLiteral("CSV formula-like filenames are literal text"), error);
    check.expect(exportFolderHtmlReport(folder, roots, html, &error) && get(html).contains("Different"), QStringLiteral("folder HTML exports result rows"), error);
    folder.aborted = true;
    check.expect(!exportFolderHtmlReport(folder, roots, html, &error), QStringLiteral("incomplete folder result cannot be exported as complete"));
}

QByteArray wordXml(const QByteArray &body)
{
    return "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>" + body + "</w:body></w:document>";
}

QByteArray odfXml(const QByteArray &body)
{
    return "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\"><office:body>" + body + "</office:body></office:document-content>";
}

QByteArray pdfFixture()
{
    const QByteArray text = "BT /F1 12 Tf 72 720 Td (MergeDesk PDF fixture) Tj ET\n";
    const QList<QByteArray> objects {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length " + QByteArray::number(text.size()) + " >>\nstream\n" + text + "endstream"
    };
    QByteArray pdf = "%PDF-1.4\n";
    QList<int> offsets;
    for (int i = 0; i < objects.size(); ++i) { offsets.append(pdf.size()); pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n"; }
    const int xref = pdf.size();
    pdf += "xref\n0 6\n0000000000 65535 f \n";
    for (const int offset : offsets) pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    return pdf + "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
}

void documentTests(const QTemporaryDir &dir, Checks &check)
{
    QString error;
    DocumentText text;
    const QString docx = dir.filePath(QStringLiteral("unicode & original.docx"));
    const QByteArray word = wordXml("<w:p><w:pPr><w:tabs><w:tab w:val=\"left\" w:pos=\"720\"/><w:tab w:val=\"right\" w:pos=\"1440\"/></w:tabs></w:pPr><w:r><w:t>Hello &amp; caf\xc3\xa9</w:t><w:tab/><w:t>world</w:t></w:r></w:p><w:p><w:del><w:r><w:delText>deleted</w:delText></w:r></w:del><w:ins><w:r><w:t>accepted</w:t></w:r></w:ins></w:p>");
    check.expect(zip(docx, {{QStringLiteral("word/document.xml"), word}, {QStringLiteral("word/vbaProject.bin"), "not executed"}}), QStringLiteral("create DOCX test fixture"));
    const auto original = get(docx);
    check.expect(convertDocument(docx, &text, &error), QStringLiteral("native DOCX extraction"), error);
    check.expect(text.text == QStringLiteral("Hello & café\tworld\naccepted\n") && !text.warnings.isEmpty(), QStringLiteral("DOCX text, Unicode, content tabs, formatting tab stops and tracked deletions"), text.text);
    check.expect(get(docx) == original, QStringLiteral("document conversion preserves original bytes"));
    ConversionOptions small; small.maximumTextBytes = 4;
    check.expect(!convertDocument(docx, &text, &error, small), QStringLiteral("document output budget enforced"));
    const QString bad = dir.filePath(QStringLiteral("bad.docx"));
    zip(bad, {{QStringLiteral("word/document.xml"), "<!DOCTYPE document [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><document>&x;</document>"}});
    check.expect(!convertDocument(bad, &text, &error), QStringLiteral("document DTD rejected"));
    zip(bad, {{QStringLiteral("word/document.xml"), word}, {QStringLiteral("../escape.txt"), "unsafe"}});
    check.expect(!convertDocument(bad, &text, &error), QStringLiteral("document traversal entry rejected"));
    zip(bad, {{QStringLiteral("word/document.xml"), "<not-a-document/>"}});
    check.expect(!convertDocument(bad, &text, &error), QStringLiteral("wrong document XML rejected"));
    put(bad, "not a zip package");
    check.expect(!convertDocument(bad, &text, &error), QStringLiteral("damaged document archive rejected"));
    const QString xlsx = dir.filePath(QStringLiteral("book.xlsx"));
    QMap<QString, QByteArray> excel {
        {QStringLiteral("xl/workbook.xml"), "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"Budget &amp; plan\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
        {QStringLiteral("xl/_rels/workbook.xml.rels"), "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Target=\"worksheets/sheet1.xml\" Type=\"worksheet\"/></Relationships>"},
        {QStringLiteral("xl/sharedStrings.xml"), "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><si><r><t>Annual </t></r><r><t>total</t></r></si></sst>"},
        {QStringLiteral("xl/worksheets/sheet1.xml"), "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"C1\"><f>SUM(A2:B2)</f><v>12</v></c><c r=\"D1\" t=\"b\"><v>1</v></c></row><row r=\"3\"><c r=\"B3\" t=\"inlineStr\"><is><t>inline</t></is></c></row></sheetData></worksheet>"}
    };
    check.expect(zip(xlsx, excel) && convertDocument(xlsx, &text, &error), QStringLiteral("native XLSX extraction"), error);
    check.expect(text.text.contains(QStringLiteral("A1\tAnnual total")) && text.text.contains(QStringLiteral("C1\t=SUM(A2:B2) [cached=12]")) && text.text.contains(QStringLiteral("D1\tTRUE")) && text.text.contains(QStringLiteral("B3\tinline")), QStringLiteral("XLSX sparse cells, rich strings, formulas and booleans"), text.text);
    excel[QStringLiteral("xl/sharedStrings.xml")] = "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>";
    zip(xlsx, excel);
    check.expect(!convertDocument(xlsx, &text, &error), QStringLiteral("invalid XLSX shared-string reference rejected"));
    const QString pptx = dir.filePath(QStringLiteral("slides.pptx"));
    const QByteArray slidePrefix = "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><p:cSld><a:p><a:r><a:t>";
    const QByteArray slideSuffix = "</a:t></a:r></a:p></p:cSld></p:sld>";
    QMap<QString, QByteArray> slides {
        {QStringLiteral("ppt/presentation.xml"), "<p:presentation xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><p:sldIdLst><p:sldId id=\"256\" r:id=\"second\"/><p:sldId id=\"257\" r:id=\"first\"/></p:sldIdLst></p:presentation>"},
        {QStringLiteral("ppt/_rels/presentation.xml.rels"), "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"first\" Target=\"slides/slide1.xml\" Type=\"slide\"/><Relationship Id=\"second\" Target=\"slides/slide2.xml\" Type=\"slide\"/></Relationships>"},
        {QStringLiteral("ppt/slides/slide1.xml"), slidePrefix + "First file, second slide" + slideSuffix},
        {QStringLiteral("ppt/slides/slide2.xml"), slidePrefix + "Second file, first slide" + slideSuffix}
    };
    check.expect(zip(pptx, slides) && convertDocument(pptx, &text, &error), QStringLiteral("native PPTX extraction"), error);
    check.expect(text.text.indexOf(QStringLiteral("Second file, first slide")) < text.text.indexOf(QStringLiteral("First file, second slide")), QStringLiteral("PPTX relationship order respected"), text.text);
    const QByteArray tabbedParagraph = "<a:p><a:pPr><a:tabLst><a:tab pos=\"0\" algn=\"l\"/><a:tab pos=\"914400\" algn=\"l\"/></a:tabLst></a:pPr><a:r><a:t>one&#9;two</a:t></a:r><a:br/><a:r><a:t>Next line</a:t></a:r></a:p>";
    slides[QStringLiteral("ppt/slides/slide1.xml")] = "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><p:cSld>" + tabbedParagraph + "</p:cSld></p:sld>";
    slides[QStringLiteral("ppt/slides/_rels/slide1.xml.rels")] = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"notes\" Target=\"../notesSlides/notesSlide1.xml\" Type=\"notesSlide\"/></Relationships>";
    slides[QStringLiteral("ppt/notesSlides/notesSlide1.xml")] = "<p:notes xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><p:cSld>" + tabbedParagraph + "</p:cSld></p:notes>";
    const bool tabbedSlidesConverted = zip(pptx, slides) && convertDocument(pptx, &text, &error);
    check.expect(tabbedSlidesConverted
        && text.text == QStringLiteral("# Slide 1\nSecond file, first slide\n\n# Slide 2\none\ttwo\nNext line\n# Notes\none\ttwo\nNext line\n\n"),
        QStringLiteral("PPTX slide and note tab stops are formatting while literal tabs and line breaks survive"), error + text.text);
    const QString odt = dir.filePath(QStringLiteral("text.odt"));
    zip(odt, {{QStringLiteral("content.xml"), odfXml("<office:text><text:p>A<text:s text:c=\"3\"/>B<text:tab/>C<text:line-break/>D</text:p></office:text>")}});
    check.expect(convertDocument(odt, &text, &error) && text.text == QStringLiteral("A   B\tC\nD\n"), QStringLiteral("native ODT explicit whitespace"), error + text.text);
    const QString ods = dir.filePath(QStringLiteral("sheet.ods"));
    zip(ods, {{QStringLiteral("content.xml"), odfXml("<office:spreadsheet><table:table table:name=\"Values\"><table:table-row table:number-rows-repeated=\"2\"><table:table-cell table:number-columns-repeated=\"2\" office:value-type=\"string\"><text:p>X</text:p></table:table-cell><table:table-cell office:value-type=\"float\" office:value=\"7\"/></table:table-row><table:table-row table:number-rows-repeated=\"1000000\"><table:table-cell table:number-columns-repeated=\"1000\"/></table:table-row></table:table></office:spreadsheet>")}});
    check.expect(convertDocument(ods, &text, &error) && text.text.contains(QStringLiteral("B2\tX")) && text.text.contains(QStringLiteral("C2\t7")) && text.text.size() < 200, QStringLiteral("ODS repeated populated cells and compact blank rows"), error + text.text);
    const QString odp = dir.filePath(QStringLiteral("slides.odp"));
    zip(odp, {{QStringLiteral("content.xml"), odfXml("<office:presentation><draw:page draw:name=\"Opening\"><text:p>Welcome</text:p></draw:page></office:presentation>")}});
    check.expect(convertDocument(odp, &text, &error) && text.text.contains(QStringLiteral("Slide 1: Opening")) && text.text.contains(QStringLiteral("Welcome")), QStringLiteral("native ODP slide text"), error);
    const QString pdf = dir.filePath(QStringLiteral("optional.pdf")); put(pdf, "%PDF-1.4\n");
    ConversionOptions noTools; noTools.allowExternalTools = false;
    check.expect(!convertDocument(pdf, &text, &error, noTools) && error.contains(QStringLiteral("disabled")), QStringLiteral("optional PDF converter policy enforced"));
    if (!QStandardPaths::findExecutable(QStringLiteral("pdftotext")).isEmpty()) {
        put(pdf, pdfFixture());
        check.expect(convertDocument(pdf, &text, &error) && text.text.contains(QStringLiteral("MergeDesk PDF fixture")), QStringLiteral("real pdftotext extracts a generated PDF"), error);
    }
#ifdef Q_OS_UNIX
    if (QFileInfo::exists(QStringLiteral("/usr/bin/python3"))) {
        const QString helper = dir.filePath(QStringLiteral("slow converter"));
        put(helper, "#!/usr/bin/python3\nimport time\ntime.sleep(2)\n");
        QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        ConversionOptions limited; limited.pdfToTextExecutable = helper; limited.timeoutMilliseconds = 20;
        check.expect(!convertDocument(pdf, &text, &error, limited) && error.contains(QStringLiteral("timed out")), QStringLiteral("external converter timeout enforced"), error);
    }
#endif
    const QString legacy = dir.filePath(QStringLiteral("optional.doc")); put(legacy, "legacy");
    check.expect(!convertDocument(legacy, &text, &error, noTools), QStringLiteral("optional legacy converter policy enforced"));
    ConvertedDocuments batch;
    check.expect(convertDocuments({docx, odt}, &batch, &error), QStringLiteral("batch document comparison conversion"), error);
    const auto temporary = batch.paths;
    check.expect(batch.temporaryDirectory && temporary.size() == 2 && QFileInfo::exists(temporary.value(0)) && batch.labels[0] == docx, QStringLiteral("batch retains original labels and temporary lifetime"));
    batch = {};
    check.expect(!QFileInfo::exists(temporary.value(0)), QStringLiteral("temporary conversion files cleaned up after lifetime ends"));
    check.expect(isDocumentPath(QStringLiteral("FILE.DOCX")) && !isDocumentPath(QStringLiteral("source.cpp")), QStringLiteral("document suffix detection"));
}
} // namespace

int runFeatureTests()
{
    Checks checks;
    QTemporaryDir dir;
    if (!dir.isValid()) { QTextStream(stderr) << "Cannot create feature-test directory.\n"; return 1; }
    projectTests(dir, checks);
    settingsTests(dir, checks);
    exportTests(dir, checks);
    documentTests(dir, checks);
    QTextStream(stdout) << "MergeDesk feature tests: " << checks.passed << " passed, " << checks.failed << " failed.\n";
    return checks.failed == 0 ? 0 : 1;
}
} // namespace lm
