// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DocumentConverters.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <functional>
#include <memory>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QUrl>
#include <QXmlStreamReader>
#include <archive.h>
#include <archive_entry.h>

namespace lm {
namespace {
constexpr qint64 maxSourceBytes = 256LL * 1024 * 1024;
constexpr qint64 maxXmlPartBytes = 32LL * 1024 * 1024;
constexpr qint64 maxXmlTotalBytes = 128LL * 1024 * 1024;
constexpr int maxEntries = 20000;
constexpr int maxRows = 1048576;
constexpr int maxColumns = 16384;
using Parts = QMap<QString, QByteArray>;

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

struct TextSink {
    QString text;
    qint64 bytes = 0;
    qint64 maximum;
    QString *error;
    bool ok = true;
    bool append(const QString &value)
    {
        if (!ok) return false;
        const qint64 length = value.toUtf8().size();
        if (length > maximum - bytes) {
            ok = false;
            return fail(error, QStringLiteral("Extracted text exceeds the configured size limit."));
        }
        bytes += length;
        text += value;
        return true;
    }
    bool newline() { return append(QStringLiteral("\n")); }
    void trimTrailing(QChar ch)
    {
        while (text.endsWith(ch)) { text.chop(1); --bytes; }
    }
};

QString extension(const QString &path) { return QFileInfo(path).suffix().toLower(); }

QString packageFamily(const QString &suffix)
{
    if (QStringList{QStringLiteral("docx"), QStringLiteral("docm"), QStringLiteral("dotx"), QStringLiteral("dotm")}.contains(suffix)) return QStringLiteral("word");
    if (QStringList{QStringLiteral("xlsx"), QStringLiteral("xlsm"), QStringLiteral("xltx"), QStringLiteral("xltm")}.contains(suffix)) return QStringLiteral("excel");
    if (QStringList{QStringLiteral("pptx"), QStringLiteral("pptm"), QStringLiteral("potx"), QStringLiteral("potm")}.contains(suffix)) return QStringLiteral("powerpoint");
    if (QStringList{QStringLiteral("odt"), QStringLiteral("ods"), QStringLiteral("odp"), QStringLiteral("ott"), QStringLiteral("ots"), QStringLiteral("otp")}.contains(suffix)) return QStringLiteral("odf");
    return {};
}

bool neededPart(const QString &name, const QString &family)
{
    if (family == QStringLiteral("odf")) return name == QStringLiteral("content.xml");
    if (family == QStringLiteral("word")) {
        static const QRegularExpression pattern(QStringLiteral("^word/(document|footnotes|endnotes|comments|header[0-9]+|footer[0-9]+)\\.xml$"));
        return pattern.match(name).hasMatch();
    }
    if (family == QStringLiteral("excel")) {
        static const QRegularExpression sheets(QStringLiteral("^xl/worksheets/[^/]+\\.xml$"));
        return name == QStringLiteral("xl/workbook.xml") || name == QStringLiteral("xl/_rels/workbook.xml.rels")
            || name == QStringLiteral("xl/sharedStrings.xml") || sheets.match(name).hasMatch();
    }
    if (family == QStringLiteral("powerpoint")) {
        static const QRegularExpression slides(QStringLiteral("^ppt/(slides|notesSlides)/[^/]+\\.xml$"));
        static const QRegularExpression rels(QStringLiteral("^ppt/slides/_rels/[^/]+\\.xml\\.rels$"));
        return name == QStringLiteral("ppt/presentation.xml") || name == QStringLiteral("ppt/_rels/presentation.xml.rels")
            || slides.match(name).hasMatch() || rels.match(name).hasMatch();
    }
    return false;
}

struct ArchiveInput {
    QFile file;
    std::array<char, 65536> buffer;
};

la_ssize_t readArchive(struct archive *reader, void *opaque, const void **buffer)
{
    auto *input = static_cast<ArchiveInput *>(opaque);
    const auto count = input->file.read(input->buffer.data(), input->buffer.size());
    if (count < 0) archive_set_error(reader, EIO, "%s", input->file.errorString().toUtf8().constData());
    *buffer = input->buffer.data();
    return static_cast<la_ssize_t>(count);
}

bool loadPackage(const QString &path, const QString &family, Parts *parts, QString *error)
{
    ArchiveInput input;
    input.file.setFileName(path);
    if (!input.file.open(QIODevice::ReadOnly)) return fail(error, input.file.errorString());
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    if (!reader) return fail(error, QStringLiteral("Cannot allocate a document archive reader."));
    archive_read_support_format_zip(reader.get());
    archive_read_support_filter_none(reader.get());
    if (archive_read_open(reader.get(), &input, nullptr, readArchive, nullptr) != ARCHIVE_OK)
        return fail(error, QStringLiteral("The document is not a readable ZIP package; it may be encrypted or damaged."));
    qint64 total = 0;
    int count = 0;
    while (true) {
        archive_entry *entry = nullptr;
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        if (status != ARCHIVE_OK) {
            const char *detail = archive_error_string(reader.get());
            return fail(error, QStringLiteral("Cannot read the document package: %1").arg(detail ? QString::fromUtf8(detail) : QStringLiteral("archive error")));
        }
        if (++count > maxEntries) return fail(error, QStringLiteral("Document package exceeds 20,000 entries."));
        if (archive_entry_is_encrypted(entry) > 0) return fail(error, QStringLiteral("Encrypted documents must be decrypted before comparison."));
        const char *entryName = archive_entry_pathname_utf8(entry);
        if (!entryName) entryName = archive_entry_pathname(entry);
        if (!entryName) return fail(error, QStringLiteral("A document part has no name."));
        const QString name = QString::fromUtf8(entryName);
        if (name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
            || name.split(QLatin1Char('/')).contains(QStringLiteral(".."))
            || name.contains(QLatin1Char(':')))
            return fail(error, QStringLiteral("Unsafe document part name: %1").arg(name));
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
            return fail(error, QStringLiteral("Linked archive entries are not valid document parts."));
        if (!neededPart(name, family)) {
            if (archive_read_data_skip(reader.get()) < ARCHIVE_OK)
                return fail(error, QStringLiteral("A document archive entry could not be skipped."));
            continue;
        }
        if (archive_entry_filetype(entry) != AE_IFREG)
            return fail(error, QStringLiteral("A required document part is not a regular file."));
        if (parts->contains(name)) return fail(error, QStringLiteral("Duplicate document part: %1").arg(name));
        if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > maxXmlPartBytes)
            return fail(error, QStringLiteral("An XML part exceeds the 32 MiB limit."));
        QByteArray bytes;
        std::array<char, 65536> buffer;
        while (true) {
            const la_ssize_t n = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (n == 0) break;
            if (n < 0) return fail(error, QStringLiteral("An XML part could not be decompressed; the document may be encrypted or damaged."));
            total += n;
            if (bytes.size() + n > maxXmlPartBytes || total > maxXmlTotalBytes)
                return fail(error, QStringLiteral("Document XML exceeds the decompression size limit."));
            bytes.append(buffer.data(), n);
        }
        parts->insert(name, std::move(bytes));
    }
    return true;
}

bool nextXml(QXmlStreamReader &xml)
{
    xml.readNext();
    if (xml.isDTD() || xml.isEntityReference())
        xml.raiseError(QStringLiteral("DTD declarations and custom entity references are not accepted."));
    return !xml.hasError();
}

bool xmlResult(QXmlStreamReader &xml, const QString &part, QString *error)
{
    if (xml.hasError()) return fail(error, QStringLiteral("Malformed %1 at line %2: %3").arg(part).arg(xml.lineNumber()).arg(xml.errorString()));
    return true;
}

bool startPart(QXmlStreamReader &xml, const QString &rootName, const QString &namespacePart)
{
    while (!xml.atEnd() && !xml.isStartElement()) if (!nextXml(xml)) return false;
    if (!xml.isStartElement() || xml.name() != rootName || !xml.namespaceUri().toString().contains(namespacePart)) {
        xml.raiseError(QStringLiteral("Unexpected XML document type."));
        return false;
    }
    return true;
}

QString attr(const QXmlStreamAttributes &attributes, const QString &name)
{
    for (const auto &attribute : attributes) if (attribute.name() == name) return attribute.value().toString();
    return {};
}

bool positiveAttribute(const QXmlStreamAttributes &attributes, const QString &name, int limit, int *result, QString *error)
{
    const QString value = attr(attributes, name);
    if (value.isEmpty()) { *result = 1; return true; }
    bool ok = false; const qlonglong count = value.toLongLong(&ok);
    if (!ok || count < 1 || count > limit) return fail(error, QStringLiteral("Invalid or excessive repetition count: %1").arg(name));
    *result = static_cast<int>(count); return true;
}

QString quotedCell(QString text)
{
    if (text.contains(QLatin1Char('\t')) || text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')) || text.contains(QLatin1Char('"'))) {
        text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + text + QLatin1Char('"');
    }
    return text;
}

QString columnName(int column)
{
    QString name;
    do { --column; name.prepend(QChar(ushort('A' + column % 26))); column /= 26; } while (column > 0);
    return name;
}

bool appendWordPart(const QByteArray &bytes, const QString &name, TextSink &out)
{
    QXmlStreamReader xml(bytes);
    bool documentElement = false;
    while (!xml.atEnd() && out.ok) {
        if (!nextXml(xml)) break;
        const auto tag = xml.name();
        if (xml.isStartElement()) {
            if (!documentElement) {
                documentElement = true;
                if (!xml.namespaceUri().toString().contains(QStringLiteral("wordprocessingml"))) {
                    xml.raiseError(QStringLiteral("This is not WordprocessingML.")); break;
                }
            }
            if (tag == QStringLiteral("del") || tag == QStringLiteral("moveFrom")
                || tag == QStringLiteral("tabs")) {
                // Paragraph tab-stop definitions also contain w:tab elements;
                // only tabs within text runs contribute document characters.
                xml.skipCurrentElement();
            } else if (tag == QStringLiteral("t")) {
                out.append(xml.readElementText());
            } else if (tag == QStringLiteral("tab")) out.append(QStringLiteral("\t"));
            else if (tag == QStringLiteral("br") || tag == QStringLiteral("cr")) out.newline();
        } else if (xml.isEndElement()) {
            if (tag == QStringLiteral("p")) out.newline();
            else if (tag == QStringLiteral("tc")) { out.trimTrailing(QLatin1Char('\n')); out.append(QStringLiteral("\t")); }
            else if (tag == QStringLiteral("tr")) { out.trimTrailing(QLatin1Char('\t')); out.newline(); }
        }
    }
    return out.ok && xmlResult(xml, name, out.error);
}

bool convertWord(const Parts &parts, TextSink &out)
{
    const QString main = QStringLiteral("word/document.xml");
    if (!parts.contains(main)) return fail(out.error, QStringLiteral("The Word document has no document.xml part."));
    if (!appendWordPart(parts.value(main), main, out)) return false;
    for (auto it = parts.cbegin(); it != parts.cend(); ++it) {
        if (it.key() == main) continue;
        if (!out.append(QStringLiteral("\n# %1\n").arg(it.key()))) return false;
        if (!appendWordPart(it.value(), it.key(), out)) return false;
    }
    return true;
}

bool relationships(const Parts &parts, const QString &part, QMap<QString, QString> *map, QString *error)
{
    if (!parts.contains(part)) return fail(error, QStringLiteral("Missing relationships part: %1").arg(part));
    QXmlStreamReader xml(parts.value(part));
    if (!startPart(xml, QStringLiteral("Relationships"), QStringLiteral("relationships"))) return xmlResult(xml, part, error);
    const QString parent = part.left(part.indexOf(QStringLiteral("/_rels/")));
    while (!xml.atEnd()) {
        if (!nextXml(xml)) break;
        if (!xml.isStartElement() || xml.name() != QStringLiteral("Relationship")) continue;
        const QString id = attr(xml.attributes(), QStringLiteral("Id"));
        const QString target = QUrl::fromPercentEncoding(attr(xml.attributes(), QStringLiteral("Target")).toUtf8());
        if (attr(xml.attributes(), QStringLiteral("TargetMode")) == QStringLiteral("External")) continue;
        if (id.isEmpty() || target.isEmpty() || target.contains(QStringLiteral("://")) || target.contains(QLatin1Char('\\'))) {
            xml.raiseError(QStringLiteral("Invalid internal relationship.")); break;
        }
        const QString normalized = QDir::cleanPath(target.startsWith(QLatin1Char('/')) ? target.mid(1) : parent + QLatin1Char('/') + target);
        if (normalized == QStringLiteral("..") || normalized.startsWith(QStringLiteral("../")) || normalized.startsWith(QLatin1Char('/'))) {
            xml.raiseError(QStringLiteral("Relationship leaves the document package.")); break;
        }
        if (map->contains(id)) { xml.raiseError(QStringLiteral("Duplicate relationship identifier.")); break; }
        map->insert(id, normalized);
    }
    return xmlResult(xml, part, error);
}

bool sharedStrings(const Parts &parts, QStringList *strings, qint64 budget, QString *error)
{
    const QString part = QStringLiteral("xl/sharedStrings.xml");
    if (!parts.contains(part)) return true;
    QXmlStreamReader xml(parts.value(part));
    if (!startPart(xml, QStringLiteral("sst"), QStringLiteral("spreadsheetml"))) return xmlResult(xml, part, error);
    QString value;
    qint64 bytes = 0;
    bool inItem = false;
    while (!xml.atEnd()) {
        if (!nextXml(xml)) break;
        if (xml.isStartElement()) {
            if (xml.name() == QStringLiteral("si")) { value.clear(); inItem = true; }
            else if (xml.name() == QStringLiteral("rPh")) xml.skipCurrentElement();
            else if (inItem && xml.name() == QStringLiteral("t")) value += xml.readElementText();
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("si")) {
            bytes += value.toUtf8().size();
            if (bytes > budget || strings->size() >= 1000000) return fail(error, QStringLiteral("Spreadsheet shared strings exceed the extraction limit."));
            strings->append(value); inItem = false;
        }
    }
    return xmlResult(xml, part, error);
}

struct NamedPart { QString name; QString path; };

bool workbookSheets(const Parts &parts, QList<NamedPart> *sheets, QString *error)
{
    const QString part = QStringLiteral("xl/workbook.xml");
    if (!parts.contains(part)) return fail(error, QStringLiteral("The spreadsheet has no workbook.xml part."));
    QMap<QString, QString> rels;
    if (!relationships(parts, QStringLiteral("xl/_rels/workbook.xml.rels"), &rels, error)) return false;
    QXmlStreamReader xml(parts.value(part));
    if (!startPart(xml, QStringLiteral("workbook"), QStringLiteral("spreadsheetml"))) return xmlResult(xml, part, error);
    while (!xml.atEnd()) {
        if (!nextXml(xml)) break;
        if (xml.isStartElement() && xml.name() == QStringLiteral("sheet")) {
            const QString id = attr(xml.attributes(), QStringLiteral("id"));
            const QString name = attr(xml.attributes(), QStringLiteral("name"));
            if (!rels.contains(id) || !parts.contains(rels.value(id))) {
                xml.raiseError(QStringLiteral("A worksheet relationship is missing or unsupported (for example, a chart sheet).")); break;
            }
            sheets->append({name, rels.value(id)});
        }
    }
    if (!xmlResult(xml, part, error)) return false;
    return !sheets->isEmpty() || fail(error, QStringLiteral("The workbook contains no worksheets."));
}

bool appendWorksheet(const Parts &parts, const NamedPart &sheet, const QStringList &strings, TextSink &out)
{
    if (!out.append(QStringLiteral("# Sheet: %1\nCell\tValue\n").arg(quotedCell(sheet.name)))) return false;
    QXmlStreamReader xml(parts.value(sheet.path));
    if (!startPart(xml, QStringLiteral("worksheet"), QStringLiteral("spreadsheetml"))) return xmlResult(xml, sheet.path, out.error);
    int row = 0, column = 0;
    bool inSheetData = false;
    while (!xml.atEnd() && out.ok) {
        if (!nextXml(xml)) break;
        if (xml.isEndElement() && xml.name() == QStringLiteral("sheetData")) inSheetData = false;
        if (!xml.isStartElement()) continue;
        if (xml.name() == QStringLiteral("sheetData")) { inSheetData = true; continue; }
        if (!inSheetData) continue;
        if (xml.name() == QStringLiteral("row")) {
            const QString r = attr(xml.attributes(), QStringLiteral("r"));
            bool ok = true; row = r.isEmpty() ? row + 1 : r.toInt(&ok); column = 0;
            if (!ok || row < 1 || row > maxRows) { xml.raiseError(QStringLiteral("Invalid worksheet row.")); break; }
        } else if (xml.name() == QStringLiteral("c")) {
            ++column;
            const QString type = attr(xml.attributes(), QStringLiteral("t"));
            QString address = attr(xml.attributes(), QStringLiteral("r"));
            if (address.isEmpty()) address = columnName(column) + QString::number(row);
            static const QRegularExpression reference(QStringLiteral("^([A-Z]{1,3})([1-9][0-9]{0,6})$"));
            const auto match = reference.match(address);
            int explicitColumn = 0;
            if (match.hasMatch()) for (const QChar c : match.captured(1)) explicitColumn = explicitColumn * 26 + c.unicode() - 'A' + 1;
            if (!match.hasMatch() || explicitColumn > maxColumns || match.captured(2).toInt() > maxRows) {
                xml.raiseError(QStringLiteral("Invalid spreadsheet cell reference.")); break;
            }
            column = explicitColumn;
            QString value, formula, inlineText;
            bool hasFormula = false;
            while (!xml.atEnd()) {
                if (!nextXml(xml)) break;
                if (xml.isEndElement() && xml.name() == QStringLiteral("c")) break;
                if (!xml.isStartElement()) continue;
                if (xml.name() == QStringLiteral("v")) value = xml.readElementText();
                else if (xml.name() == QStringLiteral("t")) inlineText += xml.readElementText();
                else if (xml.name() == QStringLiteral("rPh")) xml.skipCurrentElement();
                else if (xml.name() == QStringLiteral("f")) {
                    hasFormula = true;
                    const QString shared = attr(xml.attributes(), QStringLiteral("si"));
                    formula = xml.readElementText();
                    if (formula.isEmpty() && !shared.isEmpty()) formula = QStringLiteral("SHARED[%1]").arg(shared);
                }
            }
            if (type == QStringLiteral("s") && !value.isEmpty()) {
                bool ok = false; const int index = value.toInt(&ok);
                if (!ok || index < 0 || index >= strings.size()) { xml.raiseError(QStringLiteral("Invalid shared string index.")); break; }
                value = strings.at(index);
            } else if (type == QStringLiteral("inlineStr")) value = inlineText;
            else if (type == QStringLiteral("b")) {
                if (value == QStringLiteral("1")) value = QStringLiteral("TRUE");
                else if (value == QStringLiteral("0")) value = QStringLiteral("FALSE");
                else if (!value.isEmpty()) { xml.raiseError(QStringLiteral("Invalid spreadsheet boolean.")); break; }
            }
            if (hasFormula) value = QLatin1Char('=') + formula + QStringLiteral(" [cached=%1]").arg(value);
            if (!value.isEmpty()) out.append(address + QLatin1Char('\t') + quotedCell(value) + QLatin1Char('\n'));
        }
    }
    if (!out.ok || !xmlResult(xml, sheet.path, out.error)) return false;
    return out.newline();
}

bool convertExcel(const Parts &parts, TextSink &out)
{
    QStringList strings;
    QList<NamedPart> sheets;
    if (!sharedStrings(parts, &strings, out.maximum, out.error) || !workbookSheets(parts, &sheets, out.error)) return false;
    for (const auto &sheet : sheets) if (!appendWorksheet(parts, sheet, strings, out)) return false;
    return true;
}

bool appendSlide(const QByteArray &bytes, const QString &part, TextSink &out)
{
    QXmlStreamReader xml(bytes);
    if (!startPart(xml, part.contains(QStringLiteral("notesSlides")) ? QStringLiteral("notes") : QStringLiteral("sld"), QStringLiteral("presentationml"))) return xmlResult(xml, part, out.error);
    while (!xml.atEnd() && out.ok) {
        if (!nextXml(xml)) break;
        if (xml.isStartElement()) {
            if (xml.name() == QStringLiteral("t")) out.append(xml.readElementText());
            else if (xml.name() == QStringLiteral("br")) out.newline();
            // DrawingML a:tab elements define paragraph tab stops, not text.
            // Actual tab characters are retained when reading an a:t element.
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("p")) out.newline();
    }
    return out.ok && xmlResult(xml, part, out.error);
}

bool convertPowerPoint(const Parts &parts, TextSink &out)
{
    const QString part = QStringLiteral("ppt/presentation.xml");
    if (!parts.contains(part)) return fail(out.error, QStringLiteral("The presentation has no presentation.xml part."));
    QMap<QString, QString> rels;
    if (!relationships(parts, QStringLiteral("ppt/_rels/presentation.xml.rels"), &rels, out.error)) return false;
    QXmlStreamReader xml(parts.value(part));
    if (!startPart(xml, QStringLiteral("presentation"), QStringLiteral("presentationml"))) return xmlResult(xml, part, out.error);
    QStringList slides;
    while (!xml.atEnd()) {
        if (!nextXml(xml)) break;
        if (xml.isStartElement() && xml.name() == QStringLiteral("sldId")) {
            // sldId has both an unqualified numeric id and a relationship r:id.
            QString id;
            for (const auto &a : xml.attributes()) if (a.name() == QStringLiteral("id") && !a.namespaceUri().isEmpty()) id = a.value().toString();
            if (!rels.contains(id) || !parts.contains(rels.value(id))) { xml.raiseError(QStringLiteral("A slide relationship is missing.")); break; }
            slides.append(rels.value(id));
        }
    }
    if (!xmlResult(xml, part, out.error)) return false;
    if (slides.isEmpty()) return fail(out.error, QStringLiteral("The presentation contains no slides."));
    for (int i = 0; i < slides.size(); ++i) {
        const QString slide = slides.at(i);
        if (!out.append(QStringLiteral("# Slide %1\n").arg(i + 1)) || !appendSlide(parts.value(slide), slide, out)) return false;
        const QString slideRelationships = QStringLiteral("ppt/slides/_rels/") + QFileInfo(slide).fileName() + QStringLiteral(".rels");
        if (parts.contains(slideRelationships)) {
            QMap<QString, QString> slideRels;
            if (!relationships(parts, slideRelationships, &slideRels, out.error)) return false;
            for (auto it = slideRels.cbegin(); it != slideRels.cend(); ++it) {
                if (it.value().startsWith(QStringLiteral("ppt/notesSlides/")) && parts.contains(it.value())) {
                    if (!out.append(QStringLiteral("# Notes\n")) || !appendSlide(parts.value(it.value()), it.value(), out)) return false;
                }
            }
        }
        if (!out.newline()) return false;
    }
    return true;
}

bool appendOdfSpace(QXmlStreamReader &xml, TextSink &out)
{
    int count;
    if (!positiveAttribute(xml.attributes(), QStringLiteral("c"), 100000, &count, out.error)) return false;
    return out.append(QString(count, QLatin1Char(' ')));
}

// Consumes one element, preserving ODF's explicit spaces, tabs, line breaks
// and paragraph boundaries. Whitespace outside text paragraphs is formatting
// in the XML and is not document content.
bool odfFragment(QXmlStreamReader &xml, TextSink &out)
{
    int depth = 1, paragraphs = 0;
    while (!xml.atEnd() && depth > 0 && out.ok) {
        if (!nextXml(xml)) break;
        if (xml.isStartElement()) {
            ++depth;
            const auto tag = xml.name();
            if (tag == QStringLiteral("p") || tag == QStringLiteral("h")) ++paragraphs;
            else if (paragraphs > 0 && tag == QStringLiteral("s")) { if (!appendOdfSpace(xml, out)) return false; }
            else if (paragraphs > 0 && tag == QStringLiteral("tab")) out.append(QStringLiteral("\t"));
            else if (paragraphs > 0 && tag == QStringLiteral("line-break")) out.newline();
        } else if (xml.isEndElement()) {
            if (xml.name() == QStringLiteral("p") || xml.name() == QStringLiteral("h")) { --paragraphs; out.newline(); }
            --depth;
        } else if (xml.isCharacters() && paragraphs > 0) out.append(xml.text().toString());
    }
    return out.ok && !xml.hasError();
}

struct OdfCell { int column; int repeat; QString value; };

bool readOdfCell(QXmlStreamReader &xml, QString *value, qint64 budget, QString *error)
{
    const auto attributes = xml.attributes();
    const QString type = attr(attributes, QStringLiteral("value-type"));
    const QString formula = attr(attributes, QStringLiteral("formula"));
    QString raw = attr(attributes, QStringLiteral("value"));
    if (type == QStringLiteral("date")) raw = attr(attributes, QStringLiteral("date-value"));
    else if (type == QStringLiteral("time")) raw = attr(attributes, QStringLiteral("time-value"));
    else if (type == QStringLiteral("boolean")) raw = attr(attributes, QStringLiteral("boolean-value")).toUpper();
    else if (type == QStringLiteral("string")) raw = attr(attributes, QStringLiteral("string-value"));
    TextSink visible {{}, 0, budget, error};
    if (!odfFragment(xml, visible)) return false;
    visible.trimTrailing(QLatin1Char('\n'));
    if (type == QStringLiteral("string") || raw.isEmpty()) raw = visible.text;
    if (!formula.isEmpty()) raw = formula + QStringLiteral(" [cached=%1]").arg(raw);
    *value = raw;
    return true;
}

bool appendOdfSpreadsheet(QXmlStreamReader &xml, TextSink &out)
{
    int depth = 1;
    QString sheet;
    int row = 1;
    while (!xml.atEnd() && depth > 0 && out.ok) {
        if (!nextXml(xml)) break;
        if (xml.isStartElement()) {
            ++depth;
            if (xml.name() == QStringLiteral("table")) {
                sheet = attr(xml.attributes(), QStringLiteral("name")); row = 1;
                if (!out.append(QStringLiteral("# Sheet: %1\nCell\tValue\n").arg(quotedCell(sheet)))) return false;
            } else if (xml.name() == QStringLiteral("table-row")) {
                int repeat;
                if (!positiveAttribute(xml.attributes(), QStringLiteral("number-rows-repeated"), maxRows, &repeat, out.error)) return false;
                if (row > maxRows - repeat + 1) return fail(out.error, QStringLiteral("Spreadsheet row count exceeds the extraction limit."));
                QList<OdfCell> cells;
                int column = 1;
                while (!xml.atEnd()) {
                    if (!nextXml(xml)) break;
                    if (xml.isEndElement() && xml.name() == QStringLiteral("table-row")) break;
                    if (!xml.isStartElement()) continue;
                    if (xml.name() != QStringLiteral("table-cell") && xml.name() != QStringLiteral("covered-table-cell")) continue;
                    int columns;
                    if (!positiveAttribute(xml.attributes(), QStringLiteral("number-columns-repeated"), maxColumns, &columns, out.error)) return false;
                    if (column > maxColumns - columns + 1) return fail(out.error, QStringLiteral("Spreadsheet column count exceeds the extraction limit."));
                    QString value;
                    if (!readOdfCell(xml, &value, out.maximum, out.error)) return false;
                    if (!value.isEmpty()) cells.append({column, columns, value});
                    column += columns;
                }
                // Empty repeated rows and cells advance the coordinates without
                // allocating a million blank rows. Populated repetitions are
                // expanded, with the text budget bounding the work.
                if (!cells.isEmpty()) {
                    for (int r = row; r < row + repeat; ++r)
                        for (const auto &cell : cells)
                            for (int c = cell.column; c < cell.column + cell.repeat; ++c)
                                if (!out.append(columnName(c) + QString::number(r) + QLatin1Char('\t') + quotedCell(cell.value) + QLatin1Char('\n'))) return false;
                }
                row += repeat;
                --depth; // table-row was consumed above
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QStringLiteral("table")) out.newline();
            --depth;
        }
    }
    return out.ok && !xml.hasError();
}

bool convertOdf(const Parts &parts, TextSink &out)
{
    const QString part = QStringLiteral("content.xml");
    if (!parts.contains(part)) return fail(out.error, QStringLiteral("The OpenDocument file has no content.xml part."));
    QXmlStreamReader xml(parts.value(part));
    if (!startPart(xml, QStringLiteral("document-content"), QStringLiteral("opendocument:xmlns:office"))) return xmlResult(xml, part, out.error);
    bool body = false, sawBody = false;
    int paragraphs = 0, slides = 0;
    while (!xml.atEnd() && out.ok) {
        if (!nextXml(xml)) break;
        if (xml.isStartElement()) {
            const auto tag = xml.name();
            if (tag == QStringLiteral("body")) { body = true; sawBody = true; }
            else if (body && tag == QStringLiteral("spreadsheet")) {
                if (!appendOdfSpreadsheet(xml, out)) return xml.hasError() ? xmlResult(xml, part, out.error) : false;
            } else if (body && (tag == QStringLiteral("p") || tag == QStringLiteral("h"))) ++paragraphs;
            else if (body && tag == QStringLiteral("page")) {
                ++slides;
                out.append(QStringLiteral("\n# Slide %1: %2\n").arg(slides).arg(attr(xml.attributes(), QStringLiteral("name"))));
            } else if (body && paragraphs > 0 && tag == QStringLiteral("s")) { if (!appendOdfSpace(xml, out)) return false; }
            else if (body && paragraphs > 0 && tag == QStringLiteral("tab")) out.append(QStringLiteral("\t"));
            else if (body && paragraphs > 0 && tag == QStringLiteral("line-break")) out.newline();
        } else if (xml.isEndElement()) {
            if (body && (xml.name() == QStringLiteral("p") || xml.name() == QStringLiteral("h"))) { --paragraphs; out.newline(); }
            else if (xml.name() == QStringLiteral("body")) body = false;
            else if (body && xml.name() == QStringLiteral("table-cell")) { out.trimTrailing(QLatin1Char('\n')); out.append(QStringLiteral("\t")); }
            else if (body && xml.name() == QStringLiteral("table-row")) { out.trimTrailing(QLatin1Char('\t')); out.newline(); }
        } else if (body && paragraphs > 0 && xml.isCharacters()) out.append(xml.text().toString());
    }
    if (!out.ok || !xmlResult(xml, part, out.error)) return false;
    return sawBody || fail(out.error, QStringLiteral("The OpenDocument file contains no document body."));
}

QString findHelper(const QString &configured, const QStringList &candidates)
{
    if (!configured.isEmpty()) {
        const QString found = QStandardPaths::findExecutable(configured);
        return found.isEmpty() && QFileInfo(configured).isExecutable() ? QFileInfo(configured).absoluteFilePath() : found;
    }
    for (const auto &candidate : candidates) {
        const QString found = QStandardPaths::findExecutable(candidate);
        if (!found.isEmpty()) return found;
    }
    return {};
}

bool runHelper(const QString &program, const QStringList &arguments, const QString &workingDirectory,
               const QString &output, qint64 limit, int timeout, QString *error)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(workingDirectory);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SAL_USE_VCLPLUGIN"), QStringLiteral("svp"));
    process.setProcessEnvironment(environment);
    process.setStandardInputFile(QProcess::nullDevice());
    QElapsedTimer elapsed; elapsed.start();
    process.start();
    if (!process.waitForStarted(std::min(timeout, 5000))) return fail(error, QStringLiteral("Cannot start converter: %1").arg(process.errorString()));
    QByteArray diagnostics;
    auto drain = [&] {
        process.readAllStandardOutput();
        diagnostics += process.readAllStandardError();
        if (diagnostics.size() > 65536) diagnostics = diagnostics.right(65536);
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(50);
        drain();
        if (elapsed.elapsed() > timeout || QFileInfo(output).size() > limit) {
            process.kill(); process.waitForFinished(1000);
            return fail(error, elapsed.elapsed() > timeout ? QStringLiteral("Document conversion timed out.") : QStringLiteral("Converted document exceeds the size limit."));
        }
    }
    drain();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return fail(error, QStringLiteral("Document converter failed (exit %1): %2").arg(process.exitCode()).arg(QString::fromLocal8Bit(diagnostics).trimmed().left(2000)));
    const QFileInfo result(output);
    if (!result.isFile() || result.size() > limit) return fail(error, QStringLiteral("The converter did not produce a valid output file within the size limit."));
    return true;
}

bool convertPdf(const QString &path, DocumentText *result, QString *error, const ConversionOptions &options)
{
    if (!options.allowExternalTools) return fail(error, QStringLiteral("PDF text extraction requires the optional pdftotext converter; external converters are disabled."));
    const QString helper = findHelper(options.pdfToTextExecutable, {QStringLiteral("pdftotext")});
    if (helper.isEmpty()) return fail(error, QStringLiteral("Install Poppler's pdftotext utility, or configure its executable, to compare PDF text."));
    QTemporaryDir dir;
    if (!dir.isValid()) return fail(error, QStringLiteral("Cannot create temporary PDF conversion files."));
    const QString input = dir.filePath(QStringLiteral("source.pdf"));
    const QString output = dir.filePath(QStringLiteral("output.txt"));
    if (!QFile::copy(path, input)) return fail(error, QStringLiteral("Cannot make a temporary copy of the PDF."));
    if (!runHelper(helper, {QStringLiteral("-layout"), QStringLiteral("-enc"), QStringLiteral("UTF-8"),
            QStringLiteral("-nopgbrk"), QStringLiteral("-eol"), QStringLiteral("unix"), input, output},
            dir.path(), output, options.maximumTextBytes, options.timeoutMilliseconds, error)) return false;
    QFile file(output);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(file.readAll());
    if (file.error() != QFile::NoError) return fail(error, file.errorString());
    if (decoder.hasError() || text.contains(QChar(0))) return fail(error, QStringLiteral("The PDF converter returned invalid UTF-8 text."));
    result->text = text;
    result->format = QStringLiteral("PDF text (pdftotext)");
    result->warnings.append(QStringLiteral("PDF comparison uses selectable text. Layout, graphics and scanned-page OCR are not compared."));
    if (text.trimmed().isEmpty()) result->warnings.append(QStringLiteral("The PDF has no extracted text; image-only or scanned pages may require OCR."));
    return true;
}

bool convertLegacy(const QString &path, DocumentText *result, QString *error, const ConversionOptions &options)
{
    if (!options.allowExternalTools) return fail(error, QStringLiteral("This legacy document needs optional LibreOffice conversion; external converters are disabled."));
    const QString helper = findHelper(options.officeExecutable, {QStringLiteral("libreoffice"), QStringLiteral("soffice")});
    if (helper.isEmpty()) return fail(error, QStringLiteral("Install LibreOffice, or configure its executable, to compare legacy DOC, XLS, PPT or RTF documents."));
    const QString suffix = extension(path);
    const QString target = suffix == QStringLiteral("xls") ? QStringLiteral("xlsx")
        : suffix == QStringLiteral("ppt") ? QStringLiteral("pptx") : QStringLiteral("docx");
    QTemporaryDir dir;
    if (!dir.isValid()) return fail(error, QStringLiteral("Cannot create temporary Office conversion files."));
    QDir(dir.path()).mkdir(QStringLiteral("output"));
    const QString input = dir.filePath(QStringLiteral("source.") + suffix);
    const QString outputDir = dir.filePath(QStringLiteral("output"));
    const QString output = QDir(outputDir).filePath(QStringLiteral("source.") + target);
    if (!QFile::copy(path, input)) return fail(error, QStringLiteral("Cannot make a temporary copy of the legacy document."));
    // A private profile prevents attaching to an existing Office session or
    // modifying the user's normal LibreOffice settings.
    const QString profileDirectory = dir.filePath(QStringLiteral("profile"));
    if (!QDir().mkpath(QDir(profileDirectory).filePath(QStringLiteral("user"))))
        return fail(error, QStringLiteral("Cannot create the private Office profile."));
    // These settings are defined by LibreOffice's Office/Common schema under
    // Security/Scripting. They disable macros and active OLE/DDE content in
    // the converter's private profile, regardless of the user's normal setup.
    const QByteArray profileSettings =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<oor:items xmlns:oor=\"http://openoffice.org/2001/registry\">"
        "<item oor:path=\"/org.openoffice.Office.Common/Security/Scripting\">"
        "<prop oor:name=\"MacroSecurityLevel\" oor:op=\"fuse\"><value>3</value></prop>"
        "<prop oor:name=\"DisableMacrosExecution\" oor:op=\"fuse\"><value>true</value></prop>"
        "<prop oor:name=\"DisableActiveContent\" oor:op=\"fuse\"><value>true</value></prop>"
        "</item></oor:items>";
    QSaveFile profileFile(QDir(profileDirectory).filePath(QStringLiteral("user/registrymodifications.xcu")));
    if (!profileFile.open(QIODevice::WriteOnly) || profileFile.write(profileSettings) != profileSettings.size() || !profileFile.commit())
        return fail(error, QStringLiteral("Cannot configure the private Office profile."));
    const QString profile = QString::fromUtf8(QUrl::fromLocalFile(profileDirectory).toEncoded());
    if (!runHelper(helper, {QStringLiteral("-env:UserInstallation=") + profile,
            QStringLiteral("--headless"), QStringLiteral("--nologo"), QStringLiteral("--nodefault"),
            QStringLiteral("--nolockcheck"), QStringLiteral("--norestore"), QStringLiteral("--convert-to"), target,
            QStringLiteral("--outdir"), outputDir, input},
            dir.path(), output, maxSourceBytes, options.timeoutMilliseconds, error)) return false;
    ConversionOptions native = options; native.allowExternalTools = false;
    if (!convertDocument(output, result, error, native)) return false;
    result->format = suffix.toUpper() + QStringLiteral(" text (LibreOffice)");
    result->warnings.prepend(QStringLiteral("Legacy document text was converted by LibreOffice in a private temporary profile; the source was not changed."));
    return true;
}
} // namespace

QStringList supportedDocumentExtensions()
{
    return {QStringLiteral("docx"), QStringLiteral("docm"), QStringLiteral("dotx"), QStringLiteral("dotm"),
        QStringLiteral("xlsx"), QStringLiteral("xlsm"), QStringLiteral("xltx"), QStringLiteral("xltm"),
        QStringLiteral("pptx"), QStringLiteral("pptm"), QStringLiteral("potx"), QStringLiteral("potm"),
        QStringLiteral("odt"), QStringLiteral("ods"), QStringLiteral("odp"),
        QStringLiteral("ott"), QStringLiteral("ots"), QStringLiteral("otp"),
        QStringLiteral("pdf"), QStringLiteral("doc"), QStringLiteral("xls"), QStringLiteral("ppt"), QStringLiteral("rtf")};
}

bool isDocumentPath(const QString &path) { return supportedDocumentExtensions().contains(extension(path)); }

bool convertDocument(const QString &path, DocumentText *result, QString *error, const ConversionOptions &options)
{
    if (error) error->clear();
    if (!result) return fail(error, QStringLiteral("No conversion destination was supplied."));
    if (options.maximumTextBytes < 1 || options.maximumTextBytes > maxSourceBytes || options.timeoutMilliseconds < 1 || options.timeoutMilliseconds > 600000)
        return fail(error, QStringLiteral("Converter limits must be 1 byte–256 MiB of text and 1–600,000 milliseconds."));
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) return fail(error, QStringLiteral("Cannot read document: %1").arg(path));
    if (info.size() > maxSourceBytes) return fail(error, QStringLiteral("The document exceeds the 256 MiB input limit."));
    const QString suffix = extension(path);
    if (!supportedDocumentExtensions().contains(suffix)) return fail(error, QStringLiteral("This document format has no configured text converter: %1").arg(suffix));
    DocumentText converted;
    if (suffix == QStringLiteral("pdf")) {
        if (!convertPdf(info.absoluteFilePath(), &converted, error, options)) return false;
    } else if (QStringList{QStringLiteral("doc"), QStringLiteral("xls"), QStringLiteral("ppt"), QStringLiteral("rtf")}.contains(suffix)) {
        if (!convertLegacy(info.absoluteFilePath(), &converted, error, options)) return false;
    } else {
        const QString family = packageFamily(suffix);
        Parts parts;
        if (!loadPackage(info.absoluteFilePath(), family, &parts, error)) return false;
        TextSink out {{}, 0, options.maximumTextBytes, error};
        bool ok = false;
        if (family == QStringLiteral("word")) ok = convertWord(parts, out);
        else if (family == QStringLiteral("excel")) ok = convertExcel(parts, out);
        else if (family == QStringLiteral("powerpoint")) ok = convertPowerPoint(parts, out);
        else if (family == QStringLiteral("odf")) ok = convertOdf(parts, out);
        if (!ok) return false;
        converted.text = std::move(out.text);
        converted.format = suffix.toUpper() + QStringLiteral(" text");
        converted.warnings.append(QStringLiteral("Document comparison extracts text and stored cell values. Formatting, graphics, embedded objects and visual layout are not compared; macros and external links are not executed."));
        if (family == QStringLiteral("excel") || suffix == QStringLiteral("ods") || suffix == QStringLiteral("ots"))
            converted.warnings.append(QStringLiteral("Spreadsheet cells are listed by address. Formulas and cached values are shown without recalculation; numeric dates retain their stored representation."));
        if (family == QStringLiteral("word"))
            converted.warnings.append(QStringLiteral("Tracked deletions and moved-from text are excluded. Available headers, footers, notes and comments follow the main text."));
    }
    *result = std::move(converted);
    return true;
}

bool convertDocuments(const QStringList &paths, ConvertedDocuments *result, QString *error, const ConversionOptions &options)
{
    if (error) error->clear();
    if (!result) return fail(error, QStringLiteral("No conversion destination was supplied."));
    if (paths.size() < 2 || paths.size() > 3) return fail(error, QStringLiteral("Document comparison needs two or three paths."));
    ConvertedDocuments converted;
    converted.paths = paths;
    converted.labels = paths;
    for (int side = 0; side < paths.size(); ++side) {
        if (!isDocumentPath(paths.at(side))) continue;
        if (!converted.temporaryDirectory) {
            converted.temporaryDirectory = std::make_shared<QTemporaryDir>();
            if (!converted.temporaryDirectory->isValid()) return fail(error, QStringLiteral("Cannot create a temporary document comparison directory."));
        }
        DocumentText document;
        if (!convertDocument(paths.at(side), &document, error, options)) return false;
        const QString target = converted.temporaryDirectory->filePath(QStringLiteral("pane-%1.txt").arg(side + 1));
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
        const QByteArray bytes = document.text.toUtf8();
        if (file.write(bytes) != bytes.size() || !file.commit()) return fail(error, file.errorString());
        converted.paths[side] = target;
        for (const auto &warning : document.warnings)
            converted.warnings.append(QFileInfo(paths.at(side)).fileName() + QStringLiteral(": ") + warning);
    }
    *result = std::move(converted);
    return true;
}
} // namespace lm
