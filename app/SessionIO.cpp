// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionIO.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStringDecoder>
#include <limits>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace lm {
namespace {
constexpr qint64 maxProjectBytes = 16 * 1024 * 1024;
struct OptionTag { const char *tag; const char *key; bool boolean; };
constexpr OptionTag optionTags[] = {
    {"white-spaces", "Settings/IgnoreSpace", false},
    {"ignore-blank-lines", "Settings/IgnoreBlankLines", true},
    {"ignore-case", "Settings/IgnoreCase", true},
    {"ignore-carriage-return-diff", "Settings/IgnoreEol", true},
    {"ignore-numbers", "Settings/IgnoreNumbers", true},
    {"ignore-codepage-diff", "Settings/IgnoreCodepage", true},
    {"ignore-comment-diff", "Settings/FilterCommentsLines", true},
    {"ignore-missing-trailing-eol", "Settings/IgnoreMissingTrailingEol", true},
    {"ignore-line-breaks", "Settings/IgnoreLineBreaks", true}
};

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool writeAtomically(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return fail(error, out.errorString());
    if (out.write(bytes) != bytes.size() || !out.commit()) return fail(error, out.errorString());
    return true;
}

bool isForeignAbsolute(const QString &path)
{
    static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    return drive.match(path).hasMatch() || path.startsWith(QStringLiteral("\\\\"));
}

QString resolvePath(const QString &path, const QDir &base, QStringList &warnings)
{
    if (path.isEmpty() || path.contains(QStringLiteral("://")))
        return path;
#ifndef Q_OS_WIN
    if (isForeignAbsolute(path)) {
        warnings.append(QStringLiteral("Windows path must be remapped on this platform: %1").arg(path));
        return path;
    }
#endif
    if (QDir::isAbsolutePath(path)) return QDir::cleanPath(path);
    return base.absoluteFilePath(QString(path).replace(QLatin1Char('\\'), QLatin1Char('/')));
}

QString storedPath(const QString &path, const QDir &base, bool relative)
{
    if (!relative || path.isEmpty() || path.contains(QStringLiteral("://"))) return path;
#ifndef Q_OS_WIN
    if (isForeignAbsolute(path)) return path;
#endif
    return QDir::fromNativeSeparators(base.relativeFilePath(QFileInfo(path).absoluteFilePath()));
}

void copyToken(QXmlStreamReader &in, QXmlStreamWriter &out)
{
    if (in.isStartElement()) {
        out.writeStartElement(in.qualifiedName().toString());
        for (const auto &ns : in.namespaceDeclarations())
            out.writeNamespace(ns.namespaceUri().toString(), ns.prefix().toString());
        out.writeAttributes(in.attributes());
    } else if (in.isEndElement()) out.writeEndElement();
    else if (in.isCharacters()) out.writeCharacters(in.text().toString());
    else if (in.isComment()) out.writeComment(in.text().toString());
}

QString readUnknownElement(QXmlStreamReader &in)
{
    QString xml;
    QXmlStreamWriter out(&xml);
    int depth = 0;
    do {
        if (in.isStartElement()) ++depth;
        if (in.isEndElement()) --depth;
        if (in.isDTD() || in.isEntityReference()) {
            in.raiseError(QStringLiteral("DTD and custom entity references are not supported."));
            break;
        }
        copyToken(in, out);
        if (depth == 0) break;
        in.readNext();
    } while (!in.atEnd());
    return xml;
}

bool readBoolean(const QString &value, bool *result)
{
    const auto v = value.trimmed().toLower();
    if (v == QStringLiteral("1") || v == QStringLiteral("true")) { *result = true; return true; }
    if (v == QStringLiteral("0") || v == QStringLiteral("false")) { *result = false; return true; }
    return false;
}

bool readComparison(QXmlStreamReader &xml, const QDir &base, ProjectSession &out)
{
    CommandLine cmd;
    QMap<QString, QString> extras;
    QMap<QString, QString> fields;
    while (xml.readNextStartElement()) {
        const QString tag = xml.name().toString();
        if (fields.contains(tag) || extras.contains(tag)) {
            xml.raiseError(QStringLiteral("Duplicate project field: %1").arg(tag));
            return false;
        }
        const bool isOption = std::any_of(std::begin(optionTags), std::end(optionTags),
            [&](const OptionTag &item) { return tag == QLatin1String(item.tag); });
        const QStringList known {QStringLiteral("left"), QStringLiteral("middle"), QStringLiteral("right"),
            QStringLiteral("left-desc"), QStringLiteral("middle-desc"), QStringLiteral("right-desc"),
            QStringLiteral("left-readonly"), QStringLiteral("middle-readonly"), QStringLiteral("right-readonly"),
            QStringLiteral("subfolders"), QStringLiteral("filter"), QStringLiteral("window-type"),
            QStringLiteral("table-delimiter"), QStringLiteral("table-quote"), QStringLiteral("table-allownewlinesinquotes"),
            QStringLiteral("compare-method"), QStringLiteral("mergedesk-kind"), QStringLiteral("mergedesk-output")};
        if (isOption || known.contains(tag)) fields.insert(tag, xml.readElementText());
        else {
            extras.insert(tag, readUnknownElement(xml));
            out.warnings.append(QStringLiteral("Project field '%1' was retained but is not applied by MergeDesk.").arg(tag));
        }
    }
    if (xml.hasError()) return false;
    if (!fields.contains(QStringLiteral("left")) || !fields.contains(QStringLiteral("right"))) {
        xml.raiseError(QStringLiteral("Each comparison needs left and right paths."));
        return false;
    }
    cmd.paths.append(resolvePath(fields.value(QStringLiteral("left")), base, out.warnings));
    if (fields.contains(QStringLiteral("middle")))
        cmd.paths.append(resolvePath(fields.value(QStringLiteral("middle")), base, out.warnings));
    cmd.paths.append(resolvePath(fields.value(QStringLiteral("right")), base, out.warnings));
    cmd.leftDesc = fields.value(QStringLiteral("left-desc"));
    cmd.middleDesc = fields.value(QStringLiteral("middle-desc"));
    cmd.rightDesc = fields.value(QStringLiteral("right-desc"));
    cmd.fileFilter = fields.value(QStringLiteral("filter"));
    cmd.outputPath = resolvePath(fields.value(QStringLiteral("mergedesk-output")), base, out.warnings);
    const auto boolean = [&](const char *name, const std::function<void(bool)> &set) {
        const QString key = QLatin1String(name);
        if (!fields.contains(key)) return true;
        bool value;
        if (!readBoolean(fields.value(key), &value)) {
            xml.raiseError(QStringLiteral("Invalid boolean value for %1.").arg(key));
            return false;
        }
        set(value); return true;
    };
    if (!boolean("left-readonly", [&](bool v){ if(v) cmd.leftFlags |= CommandLine::ReadOnly; })
        || !boolean("middle-readonly", [&](bool v){ if(v) cmd.middleFlags |= CommandLine::ReadOnly; })
        || !boolean("right-readonly", [&](bool v){ if(v) cmd.rightFlags |= CommandLine::ReadOnly; })
        || !boolean("subfolders", [&](bool v){ cmd.recursive = v; })
        || !boolean("table-allownewlinesinquotes", [&](bool v){ cmd.tableAllowNewlinesInQuotes = v; })) return false;
    for (const auto &option : optionTags) {
        const QString key = QLatin1String(option.tag);
        if (!fields.contains(key)) continue;
        QString value = fields.value(key).trimmed();
        if (option.boolean) {
            bool parsed;
            if (!readBoolean(value, &parsed)) { xml.raiseError(QStringLiteral("Invalid option: %1").arg(key)); return false; }
            value = parsed ? QStringLiteral("1") : QStringLiteral("0");
        } else {
            bool ok = false; const int parsed = value.toInt(&ok);
            if (!ok || parsed < 0 || parsed > 2) { xml.raiseError(QStringLiteral("Invalid whitespace option.")); return false; }
        }
        cmd.options.append({QLatin1String(option.key), value});
    }
    if (fields.contains(QStringLiteral("compare-method"))) {
        bool ok = false; const int method = fields.value(QStringLiteral("compare-method")).toInt(&ok);
        if (!ok || method < 0 || method > 6) { xml.raiseError(QStringLiteral("Unsupported comparison method.")); return false; }
        cmd.compareMethod = method;
    }
    if (fields.contains(QStringLiteral("window-type"))) {
        bool ok = false; const int type = fields.value(QStringLiteral("window-type")).toInt(&ok);
        if (!ok || type < -1 || type > 5) { xml.raiseError(QStringLiteral("Unsupported comparison window type.")); return false; }
        if (type >= 0) cmd.windowType = static_cast<CommandLine::WindowType>(type);
    }
    if (fields.value(QStringLiteral("mergedesk-kind")) == QStringLiteral("folder")) cmd.windowType = CommandLine::WindowType::Folder;
    for (const char *name : {"table-delimiter", "table-quote"}) {
        const QString key = QLatin1String(name);
        if (!fields.contains(key)) continue;
        const QString value = fields.value(key);
        if (value.size() > 1) { xml.raiseError(QStringLiteral("%1 must contain at most one character.").arg(key)); return false; }
        const QChar character = value.isEmpty() ? QChar() : value.front();
        if (key == QStringLiteral("table-delimiter")) cmd.tableDelimiter = character;
        else cmd.tableQuote = character;
    }
    out.comparisons.append(cmd); out.extraFields.append(extras);
    return true;
}

enum class SettingType { Boolean, Integer, Text };
struct SettingMapping {
    const char *source;
    const char *destination;
    SettingType type;
    int minimum = 0;
    int maximum = std::numeric_limits<int>::max();
};
// These are the portable preferences initialized in EngineOptions.cpp and
// the WinMerge-to-Qt mappings in Settings.cpp. Unknown Windows preferences
// are reported rather than copied under an ineffective QSettings key.
const SettingMapping settingsMappings[] = {
    {"Settings/IgnoreSpace", nullptr, SettingType::Integer, 0, 2},
    {"Settings/IgnoreBlankLines", nullptr, SettingType::Boolean},
    {"Settings/IgnoreCase", nullptr, SettingType::Boolean},
    {"Settings/IgnoreNumbers", nullptr, SettingType::Boolean},
    {"Settings/IgnoreEol", nullptr, SettingType::Boolean},
    {"Settings/IgnoreCodepage", nullptr, SettingType::Boolean},
    {"Settings/FilterCommentsLines", nullptr, SettingType::Boolean},
    {"Settings/IgnoreMissingTrailingEol", nullptr, SettingType::Boolean},
    {"Settings/IgnoreLineBreaks", nullptr, SettingType::Boolean},
    {"Settings/CompletelyBlankOutIgnoredChanges", nullptr, SettingType::Boolean},
    {"Settings/DiffAlgorithm", nullptr, SettingType::Integer, 0, 4},
    {"Settings/IndentHeuristic", nullptr, SettingType::Boolean},
    {"Settings/MovedBlocks", nullptr, SettingType::Boolean},
    {"Settings/CompMethod2", nullptr, SettingType::Integer, 0, 6},
    {"Settings/Recurse", nullptr, SettingType::Boolean},
    {"Settings/CodepageDefaultMode", nullptr, SettingType::Integer, 0, 2},
    {"Settings/CodepageDefaultCustomValue", nullptr, SettingType::Integer, 0, 65535},
    {"Settings/CodepageDetection", nullptr, SettingType::Integer},
    {"Settings/PluginsEnabled", nullptr, SettingType::Boolean},
    {"Settings/IgnoreRegExp", nullptr, SettingType::Boolean},
    {"Settings/SubstitutionFiltersEnabled", nullptr, SettingType::Boolean},
    {"Settings/HiliteSyntax", nullptr, SettingType::Boolean},
    {"Settings/HiliteSyntaxMode", nullptr, SettingType::Integer, 0, 2},
    {"Settings/AllowMixedEOL", nullptr, SettingType::Boolean},
    {"Settings/TabSize", nullptr, SettingType::Integer, 0, 64},
    {"Settings/TabType", nullptr, SettingType::Integer, 0, 1},
    {"Settings/AutomaticRescan", nullptr, SettingType::Boolean},
    {"Settings/ViewWhitespace", nullptr, SettingType::Boolean},
    {"Settings/ViewEOL", nullptr, SettingType::Boolean},
    {"Settings/CopyGranularity", nullptr, SettingType::Integer, 0, 3},
    {"Settings/HiliteWordDiff", nullptr, SettingType::Boolean},
    {"Settings/BreakOnWords", nullptr, SettingType::Boolean},
    {"Settings/BreakType", nullptr, SettingType::Integer, 0, 1},
    {"Settings/HiliteBreakSeparators", nullptr, SettingType::Text},
    {"Settings/ScrollToFirst", "General/ScrollToFirstDiff", SettingType::Boolean},
    {"Settings/ScrollToFirstInlineDiff", "General/ScrollToFirstInlineDiff", SettingType::Boolean},
    {"Settings/ShowFileDialog", "General/ShowSelectorAtStartup", SettingType::Boolean},
    {"Settings/AskClosingMultipleWindows", "General/AskCloseMultipleTabs", SettingType::Boolean},
    {"Settings/CloseWithEsc", "General/CloseWithEsc", SettingType::Integer, 0, 3},
    {"Settings/VerifyOpenPaths", "General/VerifyOpenPaths", SettingType::Boolean},
    {"Settings/PreserveFiletimes", "General/PreserveFileTime", SettingType::Boolean},
    {"Settings/CloseWithOK", "General/CloseSelectorOnCompare", SettingType::Boolean},
    {"Settings/AutoCompleteSource", "General/AutoCompleteSource", SettingType::Integer, 0, 3},
    {"Settings/AutoReloadModifiedFiles", "General/AutoReloadModifiedFiles", SettingType::Integer, 0, 3},
    {"Backup/EnableFile", "Backup/FileCompare", SettingType::Boolean},
    {"Settings/CSVFilePatterns", "TableCompare/CSVFilePatterns", SettingType::Text},
    {"Settings/CSVDelimiterCharacter", "TableCompare/CSVDelimiterCharacter", SettingType::Text},
    {"Settings/TSVFilePatterns", "TableCompare/TSVFilePatterns", SettingType::Text},
    {"Settings/DSVFilePatterns", "TableCompare/DSVFilePatterns", SettingType::Text},
    {"Settings/DSVDelimiterCharacter", "TableCompare/DSVDelimiterCharacter", SettingType::Text},
    {"Settings/TableAllowNewlinesInQuotes", "TableCompare/TableAllowNewlinesInQuotes", SettingType::Boolean},
    {"Settings/TableQuoteCharacter", "TableCompare/TableQuoteCharacter", SettingType::Text},
    {"Settings/BinaryFilePatterns", "BinaryCompare/FilePatterns", SettingType::Text},
    {"Settings/ImageFilePatterns", "ImageCompare/FilePatterns", SettingType::Text},
    {"Settings/EnableImageCompareInFolderCompare", "ImageCompare/EnableInFolderCompare", SettingType::Boolean},
    {"LineFilters/Values", nullptr, SettingType::Integer, 0, 4096},
    {"SubstitutionFilters/Values", nullptr, SettingType::Integer, 0, 4096}
};

void warning(QStringList &warnings, const QString &message)
{
    if (warnings.size() < 128) warnings.append(message);
    else if (warnings.size() == 128) warnings.append(QStringLiteral("Additional unsupported settings were skipped; only the first 128 are listed."));
}

bool decodeSettingsText(const QByteArray &bytes, QString *text, QString *error)
{
    QStringConverter::Encoding encoding = QStringConverter::Utf8;
    if (bytes.startsWith(QByteArray::fromHex("fffe"))) encoding = QStringConverter::Utf16LE;
    else if (bytes.startsWith(QByteArray::fromHex("feff"))) encoding = QStringConverter::Utf16BE;
    QStringDecoder decoder(encoding);
    *text = decoder.decode(bytes);
    if (decoder.hasError() || text->contains(QChar(0))) return fail(error, QStringLiteral("Settings files must be valid UTF-8 or BOM-marked UTF-16. Re-export legacy ANSI Registry files as Unicode."));
    return true;
}

bool readQuotedRegistry(const QString &line, qsizetype *position, QString *value)
{
    if (*position >= line.size() || line.at(*position) != QLatin1Char('"')) return false;
    ++*position;
    value->clear();
    while (*position < line.size()) {
        const QChar c = line.at((*position)++);
        if (c == QLatin1Char('"')) return true;
        if (c == QLatin1Char('\\')) {
            if (*position == line.size()) return false;
            const QChar escaped = line.at((*position)++);
            if (escaped != QLatin1Char('"') && escaped != QLatin1Char('\\')) return false;
            *value += escaped;
        } else *value += c;
    }
    return false;
}

bool registryValues(const QString &text, QMap<QString, QVariant> *values, QStringList &warnings, QString *error)
{
    auto lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty() || (lines.first().trimmed() != QStringLiteral("Windows Registry Editor Version 5.00")
        && lines.first().trimmed() != QStringLiteral("REGEDIT4")))
        return fail(error, QStringLiteral("This is not a supported exported Windows Registry file."));
    const QStringList roots {QStringLiteral("HKEY_CURRENT_USER\\Software\\Thingamahoochie\\WinMerge"),
                            QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Thingamahoochie\\WinMerge")};
    QString section;
    bool acceptedSection = false;
    int records = 0;
    for (qsizetype number = 1; number < lines.size(); ++number) {
        QString line = lines.at(number).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#'))) continue;
        if (line.startsWith(QLatin1Char('['))) {
            acceptedSection = false; section.clear();
            if (!line.endsWith(QLatin1Char(']'))) return fail(error, QStringLiteral("Malformed Registry section at line %1.").arg(number + 1));
            const QString key = line.mid(1, line.size() - 2);
            if (key.startsWith(QLatin1Char('-'))) { warning(warnings, QStringLiteral("Registry deletion directive ignored.")); continue; }
            for (const auto &root : roots) {
                if (key.compare(root, Qt::CaseInsensitive) == 0) { acceptedSection = true; break; }
                if (key.startsWith(root + QLatin1Char('\\'), Qt::CaseInsensitive)) {
                    acceptedSection = true; section = key.mid(root.size() + 1).replace(QLatin1Char('\\'), QLatin1Char('/')); break;
                }
            }
            if (!acceptedSection) warning(warnings, QStringLiteral("Registry section outside WinMerge was skipped: %1").arg(key.left(300)));
            continue;
        }
        if (!acceptedSection) continue;
        if (++records > 20000) return fail(error, QStringLiteral("Registry settings exceed 20,000 entries."));
        while (line.endsWith(QLatin1Char('\\'))) {
            line.chop(1);
            if (++number >= lines.size()) return fail(error, QStringLiteral("Incomplete Registry continuation line."));
            line += lines.at(number).trimmed();
        }
        QString name; qsizetype position = 0;
        if (line.startsWith(QLatin1Char('@'))) { warning(warnings, QStringLiteral("Default Registry values have no portable setting and were skipped.")); continue; }
        if (!readQuotedRegistry(line, &position, &name)) return fail(error, QStringLiteral("Malformed Registry value name at line %1.").arg(number + 1));
        while (position < line.size() && line.at(position).isSpace()) ++position;
        if (position >= line.size() || line.at(position++) != QLatin1Char('=')) return fail(error, QStringLiteral("Malformed Registry assignment at line %1.").arg(number + 1));
        const QString key = section.isEmpty() ? name : section + QLatin1Char('/') + name;
        QString data = line.mid(position).trimmed();
        if (data == QStringLiteral("-")) { warning(warnings, QStringLiteral("Deletion of setting '%1' was ignored.").arg(key)); continue; }
        QVariant value;
        if (data.startsWith(QLatin1Char('"'))) {
            qsizetype offset = 0; QString decoded;
            if (!readQuotedRegistry(data, &offset, &decoded) || !data.mid(offset).trimmed().isEmpty())
                return fail(error, QStringLiteral("Malformed Registry string at line %1.").arg(number + 1));
            value = decoded;
        } else if (data.startsWith(QStringLiteral("dword:"), Qt::CaseInsensitive)) {
            static const QRegularExpression dword(QStringLiteral("^dword:([0-9a-f]{1,8})$"), QRegularExpression::CaseInsensitiveOption);
            const auto match = dword.match(data);
            if (!match.hasMatch()) return fail(error, QStringLiteral("Malformed DWORD at line %1.").arg(number + 1));
            value = static_cast<qint32>(match.captured(1).toUInt(nullptr, 16));
        } else if (data.startsWith(QStringLiteral("hex(2):"), Qt::CaseInsensitive)
                   || data.startsWith(QStringLiteral("hex(1):"), Qt::CaseInsensitive)) {
            QByteArray bytes;
            const auto tokens = data.mid(7).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const auto &token : tokens) {
                const QString hex = token.trimmed(); bool ok = false; const int byte = hex.toInt(&ok, 16);
                if (!ok || hex.size() != 2 || byte < 0 || byte > 255) return fail(error, QStringLiteral("Malformed Registry string bytes at line %1.").arg(number + 1));
                bytes += char(byte);
            }
            if (bytes.size() % 2 != 0) return fail(error, QStringLiteral("Incomplete UTF-16 Registry string."));
            QStringDecoder decoder(QStringConverter::Utf16LE);
            QString decoded = decoder.decode(bytes);
            if (decoder.hasError()) return fail(error, QStringLiteral("Malformed UTF-16 Registry string."));
            if (decoded.endsWith(QChar(0))) decoded.chop(1);
            if (decoded.contains(QChar(0))) return fail(error, QStringLiteral("Embedded NUL in Registry string."));
            // REG_EXPAND_SZ stays literal: importing a setting does not expand
            // environment variables or invoke any program.
            value = decoded;
        } else {
            warning(warnings, QStringLiteral("Setting '%1' uses an unsupported Registry value type and was skipped.").arg(key));
            continue;
        }
        values->insert(key, value);
    }
    return true;
}

bool migratedValue(const QString &inputKey, const QVariant &inputValue, QString *key, QVariant *value, QStringList &warnings)
{
    QString source = inputKey;
    if (source.startsWith(QStringLiteral("WinMerge/"), Qt::CaseInsensitive)) source.remove(0, 9);
    const SettingMapping *mapping = nullptr;
    for (const auto &candidate : settingsMappings)
        if (source.compare(QLatin1String(candidate.source), Qt::CaseInsensitive) == 0
            || (candidate.destination && source.compare(QLatin1String(candidate.destination), Qt::CaseInsensitive) == 0)) { mapping = &candidate; break; }
    SettingType type = SettingType::Text;
    int minimum = 0, maximum = std::numeric_limits<int>::max();
    if (mapping) {
        *key = QLatin1String(mapping->destination ? mapping->destination : mapping->source);
        type = mapping->type; minimum = mapping->minimum; maximum = mapping->maximum;
    } else {
        static const QRegularExpression lineFilter(QStringLiteral("^LineFilters/(Filter|Enabled)([0-9]{2,4})$"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression substitutions(QStringLiteral("^SubstitutionFilters/(Enabled|UseRegExp|CaseSensitive|MatchWholeWordOnly|Pattern|Replacement)([0-9]{2,4})$"), QRegularExpression::CaseInsensitiveOption);
        const auto line = lineFilter.match(source), substitution = substitutions.match(source);
        const auto match = line.hasMatch() ? line : substitution;
        if (!match.hasMatch() || match.captured(2).toInt() >= 4096) {
            warning(warnings, QStringLiteral("Unsupported WinMerge setting was skipped: %1").arg(inputKey)); return false;
        }
        const QString property = match.captured(1).toLower();
        const QStringList canonical {QStringLiteral("Filter"), QStringLiteral("Enabled"), QStringLiteral("UseRegExp"),
            QStringLiteral("CaseSensitive"), QStringLiteral("MatchWholeWordOnly"), QStringLiteral("Pattern"), QStringLiteral("Replacement")};
        QString spelling;
        for (const auto &name : canonical) if (name.toLower() == property) spelling = name;
        *key = (line.hasMatch() ? QStringLiteral("LineFilters/") : QStringLiteral("SubstitutionFilters/")) + spelling
            + QString::number(match.captured(2).toInt()).rightJustified(2, QLatin1Char('0'));
        type = property == QStringLiteral("filter") || property == QStringLiteral("pattern") || property == QStringLiteral("replacement") ? SettingType::Text : SettingType::Boolean;
    }
    if (type == SettingType::Boolean) {
        bool parsed = false;
        if (!readBoolean(inputValue.toString(), &parsed)) { warning(warnings, QStringLiteral("Invalid boolean setting was skipped: %1").arg(inputKey)); return false; }
        *value = parsed;
    } else if (type == SettingType::Integer) {
        bool ok = false; const qlonglong parsed = inputValue.toString().toLongLong(&ok);
        if (!ok || parsed < minimum || parsed > maximum) { warning(warnings, QStringLiteral("Out-of-range integer setting was skipped: %1").arg(inputKey)); return false; }
        *value = static_cast<int>(parsed);
    } else {
        if (inputValue.metaType().id() != QMetaType::QString) { warning(warnings, QStringLiteral("Non-text setting was skipped: %1").arg(inputKey)); return false; }
        *value = inputValue.toString();
    }
    return true;
}

bool readSettings(const QString &path, QMap<QString, QVariant> *values, QStringList &warnings, QString *error)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return fail(error, input.errorString());
    const QByteArray bytes = input.read(maxProjectBytes + 1);
    if (input.error() != QFile::NoError) return fail(error, input.errorString());
    if (bytes.size() > maxProjectBytes) return fail(error, QStringLiteral("Settings exceed the 16 MiB limit."));
    QString text;
    if (!decodeSettingsText(bytes, &text, error)) return false;
    QMap<QString, QVariant> incoming;
    bool native = false;
    if (QFileInfo(path).suffix().compare(QStringLiteral("reg"), Qt::CaseInsensitive) == 0
        || text.startsWith(QStringLiteral("Windows Registry Editor Version")) || text.startsWith(QStringLiteral("REGEDIT4"))) {
        if (!registryValues(text, &incoming, warnings, error)) return false;
    } else {
        QTemporaryDir dir;
        if (!dir.isValid()) return fail(error, QStringLiteral("Cannot create a temporary settings reader."));
        const QString converted = dir.filePath(QStringLiteral("settings.ini"));
        if (!writeAtomically(converted, text.toUtf8(), error)) return false;
        QSettings ini(converted, QSettings::IniFormat); ini.setFallbacksEnabled(false);
        const auto keys = ini.allKeys();
        if (ini.status() != QSettings::NoError || keys.size() > 20000) return fail(error, QStringLiteral("The selected settings INI file is malformed or too large."));
        const QString marker = QStringLiteral("MergeDeskExport/FormatVersion");
        if (ini.contains(marker)) {
            if (ini.value(marker).toInt() != 1) return fail(error, QStringLiteral("This MergeDesk settings export version is not supported."));
            native = true;
        }
        for (const auto &key : keys) if (key != marker) incoming.insert(key, ini.value(key));
    }
    for (auto it = incoming.cbegin(); it != incoming.cend(); ++it) {
        // Imported preferences must not select a program that will execute
        // later when a document is opened. Keep the existing trusted helper
        // choices; the user can change them explicitly in the tools dialog.
        // Compare case-insensitively because Windows settings keys are not
        // case-sensitive, even if the imported INI was written elsewhere.
        if (it.key().compare(QStringLiteral("Converters/PdfToText"), Qt::CaseInsensitive) == 0
            || it.key().compare(QStringLiteral("Converters/LibreOffice"), Qt::CaseInsensitive) == 0) {
            warning(warnings, QCoreApplication::translate("SessionIO",
                "Executable setting '%1' was skipped. Choose document converters in Tools > Document Conversion Tools.").arg(it.key()));
            continue;
        }
        if (native) values->insert(it.key(), it.value());
        else {
            QString key; QVariant value;
            if (migratedValue(it.key(), it.value(), &key, &value, warnings)) values->insert(key, value);
        }
    }
    if (values->isEmpty()) return fail(error, QStringLiteral("The file contains no supported settings to import."));
    return true;
}

} // namespace

bool loadProject(const QString &path, ProjectSession *session, QString *error)
{
    if (error) error->clear();
    if (!session) return fail(error, QStringLiteral("No project destination was supplied."));
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return fail(error, input.errorString());
    if (input.size() > maxProjectBytes) return fail(error, QStringLiteral("Project exceeds the 16 MiB safety limit."));
    QXmlStreamReader xml(input.readAll());
    ProjectSession loaded;
    // Read explicitly so a DTD cannot be skipped by readNextStartElement.
    while (!xml.atEnd() && !xml.isStartElement()) {
        xml.readNext();
        if (xml.isDTD()) return fail(error, QStringLiteral("Project DTD declarations are not accepted."));
    }
    if (xml.name() != QStringLiteral("project")) return fail(error, QStringLiteral("This is not a WinMerge XML project."));
    const QDir base = QFileInfo(path).absoluteDir();
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("paths")) {
            return fail(error, QStringLiteral("Unexpected project element: %1").arg(xml.name().toString()));
        }
        if (loaded.comparisons.size() >= 128) return fail(error, QStringLiteral("Project exceeds 128 comparisons."));
        if (!readComparison(xml, base, loaded)) break;
    }
    while (!xml.atEnd()) xml.readNext();
    if (xml.hasError()) return fail(error, QStringLiteral("Invalid project XML at line %1: %2").arg(xml.lineNumber()).arg(xml.errorString()));
    if (loaded.comparisons.isEmpty()) return fail(error, QStringLiteral("The project contains no comparisons."));
    *session = std::move(loaded);
    return true;
}

bool saveProject(const QString &path, const ProjectSession &session, QString *error, bool relativePaths)
{
    if (error) error->clear();
    if (session.comparisons.isEmpty() || session.comparisons.size() > 128)
        return fail(error, QStringLiteral("A project must contain between 1 and 128 comparisons."));
    QByteArray bytes;
    QXmlStreamWriter xml(&bytes); xml.setAutoFormatting(true); xml.writeStartDocument(); xml.writeStartElement(QStringLiteral("project"));
    const QDir base = QFileInfo(path).absoluteDir();
    for (int i = 0; i < session.comparisons.size(); ++i) {
        const auto &cmd = session.comparisons.at(i);
        if (cmd.paths.size() < 2 || cmd.paths.size() > 3) return fail(error, QStringLiteral("Each comparison needs two or three paths."));
        xml.writeStartElement(QStringLiteral("paths"));
        auto text = [&](const char *name, const QString &value) { xml.writeTextElement(QLatin1String(name), value); };
        text("left", storedPath(cmd.paths.front(), base, relativePaths));
        if (cmd.paths.size() == 3) text("middle", storedPath(cmd.paths.at(1), base, relativePaths));
        text("right", storedPath(cmd.paths.back(), base, relativePaths));
        if (!cmd.leftDesc.isEmpty()) text("left-desc", cmd.leftDesc);
        if (!cmd.middleDesc.isEmpty()) text("middle-desc", cmd.middleDesc);
        if (!cmd.rightDesc.isEmpty()) text("right-desc", cmd.rightDesc);
        text("left-readonly", (cmd.leftFlags & CommandLine::ReadOnly) ? QStringLiteral("1") : QStringLiteral("0"));
        if (cmd.paths.size() == 3) text("middle-readonly", (cmd.middleFlags & CommandLine::ReadOnly) ? QStringLiteral("1") : QStringLiteral("0"));
        text("right-readonly", (cmd.rightFlags & CommandLine::ReadOnly) ? QStringLiteral("1") : QStringLiteral("0"));
        if (cmd.recursive) text("subfolders", *cmd.recursive ? QStringLiteral("1") : QStringLiteral("0"));
        if (!cmd.fileFilter.isEmpty()) text("filter", cmd.fileFilter);
        if (!cmd.outputPath.isEmpty()) text("mergedesk-output", storedPath(cmd.outputPath, base, relativePaths));
        if (cmd.windowType == CommandLine::WindowType::Folder) text("mergedesk-kind", QStringLiteral("folder"));
        else if (cmd.windowType != CommandLine::WindowType::Automatic) text("window-type", QString::number(static_cast<int>(cmd.windowType)));
        if (cmd.compareMethod) text("compare-method", QString::number(*cmd.compareMethod));
        if (cmd.tableDelimiter) text("table-delimiter", cmd.tableDelimiter->isNull() ? QString() : QString(*cmd.tableDelimiter));
        if (cmd.tableQuote) text("table-quote", cmd.tableQuote->isNull() ? QString() : QString(*cmd.tableQuote));
        if (cmd.tableAllowNewlinesInQuotes) text("table-allownewlinesinquotes", *cmd.tableAllowNewlinesInQuotes ? QStringLiteral("1") : QStringLiteral("0"));
        QMap<QString, QString> optionValues;
        for (const auto &option : cmd.options) optionValues.insert(option.first, option.second);
        for (const auto &option : optionTags)
            if (optionValues.contains(QLatin1String(option.key))) text(option.tag, optionValues.value(QLatin1String(option.key)));
        if (i < session.extraFields.size()) {
            for (auto it = session.extraFields.at(i).cbegin(); it != session.extraFields.at(i).cend(); ++it) {
                QXmlStreamReader extra(it.value());
                while (!extra.atEnd()) {
                    extra.readNext();
                    if (extra.isDTD() || extra.isEntityReference()) return fail(error, QStringLiteral("Unsupported XML in retained project field."));
                    copyToken(extra, xml);
                }
                if (extra.hasError()) return fail(error, QStringLiteral("Malformed retained project field: %1").arg(it.key()));
            }
        }
        xml.writeEndElement();
    }
    xml.writeEndElement(); xml.writeEndDocument();
    if (xml.hasError()) return fail(error, QStringLiteral("Unable to encode the project XML."));
    if (bytes.size() > maxProjectBytes) return fail(error, QStringLiteral("Project exceeds the 16 MiB size limit."));
    return writeAtomically(path, bytes, error);
}

bool exportSettings(const QString &path, QString *error)
{
    if (error) error->clear();
    QTemporaryDir dir;
    if (!dir.isValid()) return fail(error, QStringLiteral("Cannot create a temporary settings file."));
    const QString tmp = dir.filePath(QStringLiteral("settings.ini"));
    QSettings source; source.sync();
    if (source.status() != QSettings::NoError) return fail(error, QStringLiteral("Current settings could not be read."));
    {
        QSettings exported(tmp, QSettings::IniFormat); exported.setFallbacksEnabled(false);
        for (const auto &key : source.allKeys()) exported.setValue(key, source.value(key));
        exported.setValue(QStringLiteral("MergeDeskExport/FormatVersion"), 1);
        exported.sync();
        if (exported.status() != QSettings::NoError) return fail(error, QStringLiteral("Settings export could not be written."));
    }
    QFile file(tmp);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    return writeAtomically(path, file.readAll(), error);
}

bool importSettings(const QString &path, QString *error, QStringList *warnings)
{
    if (error) error->clear();
    if (warnings) warnings->clear();
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable() || info.size() > maxProjectBytes)
        return fail(error, QStringLiteral("Select a readable INI or Registry export file of at most 16 MiB."));
    QMap<QString, QVariant> imported;
    QStringList notes;
    const bool read = readSettings(path, &imported, notes, error);
    if (warnings) *warnings = notes;
    if (!read) return false;
    QSettings target; target.sync();
    if (target.status() != QSettings::NoError) return fail(error, QStringLiteral("Current settings are not writable."));
    QMap<QString, QVariant> previous;
    QStringList missing;
    for (auto it = imported.cbegin(); it != imported.cend(); ++it) {
        if (target.contains(it.key())) previous.insert(it.key(), target.value(it.key())); else missing.append(it.key());
        target.setValue(it.key(), it.value());
    }
    target.sync();
    if (target.status() != QSettings::NoError) {
        for (const auto &key : missing) target.remove(key);
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) target.setValue(it.key(), it.value());
        target.sync();
        return fail(error, QStringLiteral("Could not save imported settings; restoration of the previous values was attempted."));
    }
    return true;
}
} // namespace lm
