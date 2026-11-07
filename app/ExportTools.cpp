// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "ExportTools.h"
#include "FolderCompareDriver.h"
#include "DiffWrapper.h"
#include "DiffFileData.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTemporaryDir>

// Diffutils stores these per thread. Preserve the caller's state when a
// report is made between interactive comparisons. We never change QSettings
// or the engine's persistent options manager.
extern DECL_TLS int recursive;

namespace lm {
namespace {
constexpr qint64 maxInputBytes = 256LL * 1024 * 1024;
constexpr qint64 maxReportBytes = 512LL * 1024 * 1024;

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

String engineString(const QString &s)
{
#ifdef _UNICODE
    return s.toStdWString();
#else
    return s.toUtf8().toStdString();
#endif
}

struct SavedDiffState {
#define SAVE_INT(name) int saved_##name = ::name
    SAVE_INT(output_style); SAVE_INT(context); SAVE_INT(ignore_space_change_flag);
    SAVE_INT(ignore_all_space_flag); SAVE_INT(ignore_blank_lines_flag);
    SAVE_INT(ignore_case_flag); SAVE_INT(ignore_numbers_flag); SAVE_INT(ignore_eol_diff);
    SAVE_INT(ignore_some_changes); SAVE_INT(length_varies); SAVE_INT(always_text_flag);
    SAVE_INT(horizon_lines); SAVE_INT(heuristic); SAVE_INT(recursive);
    SAVE_INT(no_diff_means_no_output); SAVE_INT(no_details_flag);
    SAVE_INT(tab_align_flag); SAVE_INT(tab_expand_flag); SAVE_INT(paginate_flag);
    SAVE_INT(moved_blocks_flag);
#undef SAVE_INT
    char saved_line_end_char = ::line_end_char;
    char *saved_switch_string = ::switch_string;
    char *saved_file_label[2] = {::file_label[0], ::file_label[1]};
    FILE *saved_outfile = ::outfile;
    file_data saved_files[2] = {::files[0], ::files[1]};
    ~SavedDiffState()
    {
#define RESTORE_INT(name) ::name = saved_##name
        RESTORE_INT(output_style); RESTORE_INT(context); RESTORE_INT(ignore_space_change_flag);
        RESTORE_INT(ignore_all_space_flag); RESTORE_INT(ignore_blank_lines_flag);
        RESTORE_INT(ignore_case_flag); RESTORE_INT(ignore_numbers_flag); RESTORE_INT(ignore_eol_diff);
        RESTORE_INT(ignore_some_changes); RESTORE_INT(length_varies); RESTORE_INT(always_text_flag);
        RESTORE_INT(horizon_lines); RESTORE_INT(heuristic); RESTORE_INT(recursive);
        RESTORE_INT(no_diff_means_no_output); RESTORE_INT(no_details_flag);
        RESTORE_INT(tab_align_flag); RESTORE_INT(tab_expand_flag); RESTORE_INT(paginate_flag);
        RESTORE_INT(moved_blocks_flag);
#undef RESTORE_INT
        ::line_end_char = saved_line_end_char;
        ::switch_string = saved_switch_string;
        ::file_label[0] = saved_file_label[0]; ::file_label[1] = saved_file_label[1];
        ::outfile = saved_outfile;
        ::files[0] = saved_files[0]; ::files[1] = saved_files[1];
    }
};

QString normalizedPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

bool samePath(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
#ifdef Q_OS_WIN
    return normalizedPath(a).compare(normalizedPath(b), Qt::CaseInsensitive) == 0;
#else
    return normalizedPath(a) == normalizedPath(b);
#endif
}

bool validateOutput(const TextExportInput &input, const QString &destination, QString *error)
{
    if (input.paths.size() != 2)
        return fail(error, QStringLiteral("Select two panes to export; a patch describes one pair of files."));
    if (destination.isEmpty()) return fail(error, QStringLiteral("Choose an export destination."));
    for (const auto &path : input.paths)
        if (samePath(path, destination))
            return fail(error, QStringLiteral("An export cannot overwrite one of the compared source files."));
    if (!input.utf8Snapshots.isEmpty() && input.utf8Snapshots.size() != 2)
        return fail(error, QStringLiteral("Supply either both current editor buffers or neither."));
    return true;
}

bool writeBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return fail(error, output.errorString());
    if (output.write(bytes) != bytes.size() || !output.commit()) return fail(error, output.errorString());
    return true;
}

QString labelFor(const TextExportInput &input, int side)
{
    QString label = input.labels.value(side);
    if (label.isEmpty()) {
        QString basename = QFileInfo(input.paths.value(side)).fileName();
        if (basename.isEmpty()) basename = QStringLiteral("untitled.txt");
        label = (side == 0 ? QStringLiteral("a/") : QStringLiteral("b/")) + basename;
    }
    // A label is a single diff header field, never executable code. Newlines,
    // NUL and tabs could manufacture additional patch headers.
    for (QChar &ch : label)
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch == QLatin1Char('\t') || ch.isNull()) ch = QLatin1Char(' ');
    return label;
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

bool writeComparisonProbe(const QString &path, const QByteArray &source, QString *error)
{
    // WinMerge treats a lone CR as a line ending, and its patch printer
    // normalizes all endings to LF. A reversible, line-preserving probe lets
    // the existing algorithm compare physical LF lines exactly: CR becomes
    // printable data; backslashes are escaped; a prefix prevents BOM stripping.
    // The final patch's content comes from the original bytes below.
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    QByteArray buffer; buffer.reserve(65540);
    bool lineStart = true;
    for (const char byte : source) {
        if (lineStart) { buffer += '|'; lineStart = false; }
        if (byte == '\r') buffer += "\\r";
        else if (byte == '\\') buffer += "\\\\";
        else { buffer += byte; if (byte == '\n') lineStart = true; }
        if (buffer.size() >= 65536) {
            if (file.write(buffer) != buffer.size()) return fail(error, file.errorString());
            buffer.clear();
        }
    }
    if (!buffer.isEmpty() && file.write(buffer) != buffer.size()) return fail(error, file.errorString());
    if (!file.flush()) return fail(error, file.errorString());
    return true;
}

bool restorePatchBytes(QByteArray *patch, const QList<QByteArray> (&original)[2], QString *error)
{
    const auto generated = patch->split('\n');
    const QRegularExpression header(QStringLiteral("^@@ -([0-9]+)(?:,([0-9]+))? \\+([0-9]+)(?:,([0-9]+))? @@"));
    QByteArray exact;
    int left = 0, right = 0;
    bool sawHunk = false;
    auto mismatch = [&] { return fail(error, QStringLiteral("The diff engine produced a patch that cannot faithfully represent these source bytes.")); };
    for (int i = 0; i < generated.size(); ++i) {
        const auto &line = generated.at(i);
        if (i == generated.size() - 1 && line.isEmpty()) break;
        if (i < 2) { exact += line + '\n'; continue; }
        const auto match = header.match(QString::fromLatin1(line));
        if (!match.hasMatch()) return mismatch();
        sawHunk = true;
        const int oldCount = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
        const int newCount = match.captured(4).isEmpty() ? 1 : match.captured(4).toInt();
        const int oldStart = match.captured(1).toInt() - (oldCount == 0 ? 0 : 1);
        const int newStart = match.captured(3).toInt() - (newCount == 0 ? 0 : 1);
        if (oldStart < left || newStart < right || oldStart > original[0].size() || newStart > original[1].size()
            || oldStart - left != newStart - right) return mismatch();
        while (left < oldStart) if (original[0].at(left++) != original[1].at(right++)) return mismatch();
        exact += line + '\n';
        int oldRead = 0, newRead = 0;
        while (oldRead < oldCount || newRead < newCount) {
            if (++i >= generated.size() || generated.at(i).isEmpty()) return mismatch();
            const char kind = generated.at(i).at(0);
            if (kind != ' ' && kind != '-' && kind != '+') return mismatch();
            if ((kind != '+' && left >= original[0].size()) || (kind != '-' && right >= original[1].size())) return mismatch();
            const QByteArray &content = kind == '+' ? original[1].at(right) : original[0].at(left);
            if (kind == ' ' && content != original[1].at(right)) return mismatch();
            exact += kind; exact += content;
            if (!content.endsWith('\n')) {
                if (i + 1 >= generated.size() || generated.at(i + 1) != "\\ No newline at end of file") return mismatch();
                exact += "\n\\ No newline at end of file\n"; ++i;
            }
            if (kind != '+') { ++left; ++oldRead; }
            if (kind != '-') { ++right; ++newRead; }
            if (oldRead > oldCount || newRead > newCount || exact.size() > maxReportBytes) return mismatch();
        }
    }
    if (!sawHunk || original[0].size() - left != original[1].size() - right) return mismatch();
    while (left < original[0].size()) if (original[0].at(left++) != original[1].at(right++)) return mismatch();
    *patch = std::move(exact);
    return true;
}

bool createPatch(const TextExportInput &input, QByteArray *patch, QString *error, int contextLines)
{
    if (contextLines < 0 || contextLines > 10000)
        return fail(error, QStringLiteral("Context must be between 0 and 10,000 lines."));
    QTemporaryDir temporary;
    if (!temporary.isValid()) return fail(error, QStringLiteral("Cannot create temporary export files."));
    QList<QByteArray> source = input.utf8Snapshots;
    if (source.isEmpty()) {
        for (const auto &path : input.paths) {
            const QFileInfo info(path);
            if (!info.isFile() || !info.isReadable()) return fail(error, QStringLiteral("Cannot read source file: %1").arg(path));
            if (info.size() > maxInputBytes) return fail(error, QStringLiteral("A source file exceeds the 256 MiB export limit."));
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
            const QByteArray bytes = file.read(maxInputBytes + 1);
            if (file.error() != QFile::NoError) return fail(error, file.errorString());
            source.append(bytes);
        }
    }
    QStringList paths;
    QList<QByteArray> original[2];
    for (int side = 0; side < 2; ++side) {
        const auto &bytes = source.at(side);
        if (bytes.size() > maxInputBytes) return fail(error, QStringLiteral("A source buffer exceeds the 256 MiB export limit."));
        if (bytes.count('\n') + (!bytes.isEmpty() && !bytes.endsWith('\n') ? 1 : 0) > 2000000)
            return fail(error, QStringLiteral("Text export is limited to 2,000,000 lines per side."));
        if (bytes.contains('\0')) return fail(error, QStringLiteral("Binary or UTF-16 bytes cannot be exported directly as a UTF-8 text patch. Open text in an editor pane first."));
        if (!input.utf8Snapshots.isEmpty()) {
            QStringDecoder decoder(QStringDecoder::Utf8);
            const QString decoded = decoder.decode(bytes);
            Q_UNUSED(decoded);
            if (decoder.hasError()) return fail(error, QStringLiteral("Editor snapshots must contain valid UTF-8 text."));
        }
        original[side] = physicalLines(bytes);
        const QString path = temporary.filePath(QStringLiteral("pane-%1.txt").arg(side));
        if (!writeComparisonProbe(path, bytes, error)) return false;
        paths.append(path);
    }
    const QString output = temporary.filePath(QStringLiteral("comparison.patch"));
    {
        SavedDiffState saved;
        CDiffWrapper wrapper;
        DIFFOPTIONS exact {};
        exact.nDiffAlgorithm = DIFF_ALGORITHM_DEFAULT;
        exact.bIndentHeuristic = true;
        const PATCHOPTIONS options {OUTPUT_UNIFIED, contextLines, false};
        wrapper.SetPatchOptions(&options);
        wrapper.SetOptions(&exact, true);
        wrapper.EnablePlugins(false);
        wrapper.SetDetectMovedBlocks(false);
        DiffFileData data;
        data.SetDisplayFilepaths(engineString(paths.at(0)), engineString(paths.at(1)));
        if (!data.OpenFiles(engineString(paths.at(0)), engineString(paths.at(1))))
            return fail(error, QStringLiteral("The diff engine could not open the comparison files."));
        struct ScriptOwner {
            change *script = nullptr;
            ~ScriptOwner() { CDiffWrapper::FreeDiffUtilsScript(script); }
        } changes;
        int binary = 0;
        try {
            if (!wrapper.Diff2Files(&changes.script, &data, &binary, nullptr))
                return fail(error, QStringLiteral("The WinMerge diff engine could not generate the export."));
        } catch (const std::exception &exception) {
            return fail(error, QStringLiteral("The diff engine failed: %1").arg(QString::fromUtf8(exception.what())));
        } catch (...) {
            return fail(error, QStringLiteral("The diff engine could not generate the export."));
        }
        if (binary != 0) return fail(error, QStringLiteral("Unified text patches cannot represent binary files. Use a binary comparison instead."));
        if (!changes.script) {
            if (source.at(0) != source.at(1)) return fail(error, QStringLiteral("The diff engine omitted a source-byte difference; no patch was exported."));
            patch->clear(); return true;
        }

        // CDiffWrapper::SetCreatePatchFile rewrites '/' to '\\' on every
        // platform. Use the same engine's public edit-script and unified-output
        // APIs directly, with a native temporary stream, to avoid that dormant
        // Windows-only filename assumption. The comparison algorithm and
        // patch formatting remain the vendored WinMerge/diffutils code.
        FILE *native = nullptr;
#ifdef Q_OS_WIN
        const std::wstring nativePath = output.toStdWString();
        if (_wfopen_s(&native, nativePath.c_str(), L"w+b") != 0) native = nullptr;
#else
        native = std::fopen(QFile::encodeName(output).constData(), "w+b");
#endif
        if (!native) return fail(error, QStringLiteral("Cannot open the temporary patch output."));
        struct CloseStream { void operator()(FILE *stream) const { std::fclose(stream); } };
        std::unique_ptr<FILE, CloseStream> stream(native);
        ::outfile = stream.get();
        const QByteArray leftLabel = labelFor(input, 0).toUtf8(), rightLabel = labelFor(input, 1).toUtf8();
        file_data display[2] = {data.m_inf[0], data.m_inf[1]};
        display[0].name = leftLabel.constData(); display[1].name = rightLabel.constData();
        print_context_header(display, 1);
        print_context_script(changes.script, 1);
        const bool failed = std::ferror(stream.get()) != 0;
        const int closeResult = std::fclose(stream.release());
        ::outfile = nullptr;
        if (failed || closeResult != 0) return fail(error, QStringLiteral("The diff engine could not write its temporary patch."));
    }
    QFile file(output);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    if (file.size() > maxReportBytes) return fail(error, QStringLiteral("The generated patch exceeds the 512 MiB export limit."));
    *patch = file.readAll();
    if (file.error() != QFile::NoError) return fail(error, file.errorString());
    return restorePatchBytes(patch, original, error);
}

QString htmlStart(const QString &title)
{
    return QStringLiteral("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">"
        "<title>%1</title><style>"
        ":root{color-scheme:light dark;--bg:#fff;--fg:#20252b;--muted:#596776;--line:#d4dce5;"
        "--add:#daf5e5;--del:#ffe0e1;--head:#ecf1f7}"
        "@media(prefers-color-scheme:dark){:root{--bg:#181c22;--fg:#e6ebf2;--muted:#a9b6c5;"
        "--line:#3b4552;--add:#153d2b;--del:#48272c;--head:#242d38}}"
        "body{font:15px/1.5 system-ui,sans-serif;max-width:1440px;margin:32px auto;padding:0 24px;background:var(--bg);color:var(--fg)}"
        "h1{font-size:27px;margin-bottom:8px}.meta{color:var(--muted)}code,.patch{font:13px/1.55 ui-monospace,monospace}"
        "table{border-collapse:collapse;width:100%;margin:20px 0}th,td{border:1px solid var(--line);padding:7px 10px;text-align:left;vertical-align:top;overflow-wrap:anywhere}"
        "th,.head{background:var(--head)}.add{background:var(--add)}.del{background:var(--del)}"
        ".patch{border:1px solid var(--line);overflow-x:auto;margin-top:24px}.patch div{white-space:pre;min-height:1.55em;padding:0 12px}"
        ".eol{color:var(--muted);font-size:11px}.identical{color:var(--muted)}"
        "@media print{body{max-width:none;margin:0;padding:0}.patch div{white-space:pre-wrap;overflow-wrap:anywhere}}"
        "</style></head><body><h1>%1</h1>").arg(title.toHtmlEscaped());
}

QString categoryName(FolderCompareItem::Category category)
{
    switch (category) {
    case FolderCompareItem::Identical: return QStringLiteral("Identical");
    case FolderCompareItem::Different: return QStringLiteral("Different");
    case FolderCompareItem::LeftOnly: return QStringLiteral("Left only");
    case FolderCompareItem::MiddleOnly: return QStringLiteral("Middle only");
    case FolderCompareItem::RightOnly: return QStringLiteral("Right only");
    case FolderCompareItem::MissingLeft: return QStringLiteral("Missing left");
    case FolderCompareItem::MissingMiddle: return QStringLiteral("Missing middle");
    case FolderCompareItem::MissingRight: return QStringLiteral("Missing right");
    case FolderCompareItem::Skipped: return QStringLiteral("Skipped");
    case FolderCompareItem::Error: return QStringLiteral("Error");
    case FolderCompareItem::NotCompared: return QStringLiteral("Not compared");
    }
    return QStringLiteral("Unknown");
}

QString timestamp()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

bool validFolderReport(const FolderCompareResult &result, const QStringList &roots, const QString &destination, QString *error)
{
    if (!result.ok || result.aborted) return fail(error, QStringLiteral("Finish the folder comparison before exporting a report."));
    if (roots.size() != result.sides || roots.size() < 2 || roots.size() > 3)
        return fail(error, QStringLiteral("Folder report roots do not match the comparison."));
    for (const auto &item : result.items)
        for (int side = 0; side < result.sides; ++side)
            if (samePath(item.path[side], destination)) return fail(error, QStringLiteral("A report cannot overwrite a compared file."));
    return true;
}

QString csvCell(QString text)
{
    // Spreadsheet programs may evaluate untrusted filenames as formulas even
    // in quoted CSV cells. Prefix dangerous values as literal text.
    if (!text.isEmpty() && (text.front() == QLatin1Char('=') || text.front() == QLatin1Char('+')
        || text.front() == QLatin1Char('-') || text.front() == QLatin1Char('@')
        || text.front() == QLatin1Char('\t') || text.front() == QLatin1Char('\r')))
        text.prepend(QLatin1Char('\''));
    text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + text + QLatin1Char('"');
}
} // namespace

bool exportUnifiedPatch(const TextExportInput &input, const QString &destination, QString *error, int contextLines)
{
    if (error) error->clear();
    if (!validateOutput(input, destination, error)) return false;
    QByteArray patch;
    if (!createPatch(input, &patch, error, contextLines)) return false;
    return writeBytes(destination, patch, error);
}

bool exportHtmlReport(const TextExportInput &input, const QString &destination, QString *error, int contextLines)
{
    if (error) error->clear();
    if (!validateOutput(input, destination, error)) return false;
    QByteArray patch;
    if (!createPatch(input, &patch, error, contextLines)) return false;
    QString html = htmlStart(QStringLiteral("MergeDesk — text comparison"));
    html += QStringLiteral("<p class=\"meta\">Generated %1. Exact comparison; ignore rules are not applied.</p>").arg(timestamp());
    html += QStringLiteral("<table><tr><th>Left</th><td>%1</td></tr><tr><th>Right</th><td>%2</td></tr></table>")
        .arg(labelFor(input, 0).toHtmlEscaped(), labelFor(input, 1).toHtmlEscaped());
    if (!input.utf8Snapshots.isEmpty()) html += QStringLiteral("<p>Source: current editor contents, including unsaved edits.</p>");
    if (patch.isEmpty()) html += QStringLiteral("<p class=\"identical\">The compared text is identical.</p>");
    else {
        QStringDecoder decoder(QStringDecoder::Utf8);
        QString decoded = decoder.decode(patch);
        if (decoder.hasError()) {
            decoded = QString::fromLatin1(patch);
            html += QStringLiteral("<p>Legacy patch bytes are displayed as ISO-8859-1. Export a patch to preserve the original bytes.</p>");
        }
        html += QStringLiteral("<div class=\"patch\" aria-label=\"Unified comparison\">");
        const auto lines = decoded.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            QString line = lines.at(i);
            if (i == lines.size() - 1 && line.isEmpty()) break;
            const bool crlf = line.endsWith(QLatin1Char('\r'));
            if (crlf) line.chop(1);
            QString css;
            if (line.startsWith(QStringLiteral("--- ")) || line.startsWith(QStringLiteral("+++ ")) || line.startsWith(QStringLiteral("@@"))) css = QStringLiteral("head");
            else if (line.startsWith(QLatin1Char('+'))) css = QStringLiteral("add");
            else if (line.startsWith(QLatin1Char('-'))) css = QStringLiteral("del");
            html += QStringLiteral("<div class=\"%1\">%2%3</div>").arg(css, line.toHtmlEscaped(),
                crlf ? QStringLiteral("<span class=\"eol\"> ⏎ CRLF</span>") : QString());
            if (html.size() > maxReportBytes / 2) return fail(error, QStringLiteral("The HTML report exceeds the export size limit."));
        }
        html += QStringLiteral("</div>");
    }
    html += QStringLiteral("<p class=\"meta\">Generated by MergeDesk. No external resources are required.</p></body></html>\n");
    return writeBytes(destination, html.toUtf8(), error);
}

bool exportFolderHtmlReport(const FolderCompareResult &result, const QStringList &roots, const QString &destination, QString *error)
{
    if (error) error->clear();
    if (!validFolderReport(result, roots, destination, error)) return false;
    QString html = htmlStart(QStringLiteral("MergeDesk — folder comparison"));
    html += QStringLiteral("<p class=\"meta\">Generated %1. Results reflect the comparison options used for this scan.</p>").arg(timestamp());
    html += QStringLiteral("<p>%1 identical · %2 different · %3 unique</p><ol>").arg(result.identical).arg(result.different).arg(result.unique);
    for (const auto &root : roots) html += QStringLiteral("<li><code>%1</code></li>").arg(root.toHtmlEscaped());
    html += QStringLiteral("</ol><table><thead><tr><th>Relative path</th><th>Result</th><th>Kind</th>");
    for (int side = 0; side < result.sides; ++side) html += QStringLiteral("<th>Side %1 bytes</th><th>Side %1 modified (UTC)</th>").arg(side + 1);
    html += QStringLiteral("</tr></thead><tbody>");
    for (const auto &item : result.items) {
        if (item.removed) continue;
        const QString relative = item.folder.isEmpty() ? item.name : item.folder + QLatin1Char('/') + item.name;
        html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td>").arg(relative.toHtmlEscaped(), categoryName(item.category), item.isDir ? QStringLiteral("Folder") : QStringLiteral("File"));
        for (int side = 0; side < result.sides; ++side) {
            html += QStringLiteral("<td>%1</td><td>%2</td>")
                .arg(item.size[side] < 0 ? QString() : QString::number(item.size[side]),
                     item.mtime[side].isValid() ? item.mtime[side].toUTC().toString(Qt::ISODate) : QString());
        }
        html += QStringLiteral("</tr>");
        if (html.size() > maxReportBytes / 2) return fail(error, QStringLiteral("The folder report exceeds the export size limit."));
    }
    html += QStringLiteral("</tbody></table></body></html>\n");
    return writeBytes(destination, html.toUtf8(), error);
}

bool exportFolderCsvReport(const FolderCompareResult &result, const QStringList &roots, const QString &destination, QString *error)
{
    if (error) error->clear();
    if (!validFolderReport(result, roots, destination, error)) return false;
    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, file.errorString());
    auto row = [&](const QStringList &cells) {
        QStringList quoted;
        for (const auto &cell : cells) quoted.append(csvCell(cell));
        const auto bytes = (quoted.join(QLatin1Char(',')) + QStringLiteral("\r\n")).toUtf8();
        return file.write(bytes) == bytes.size();
    };
    QStringList headings {QStringLiteral("Relative path"), QStringLiteral("Result"), QStringLiteral("Kind")};
    for (int side = 0; side < result.sides; ++side)
        headings << QStringLiteral("Side %1 path").arg(side + 1) << QStringLiteral("Side %1 bytes").arg(side + 1) << QStringLiteral("Side %1 modified UTC").arg(side + 1);
    if (!row(headings)) return fail(error, file.errorString());
    for (const auto &item : result.items) {
        if (item.removed) continue;
        QStringList cells {item.folder.isEmpty() ? item.name : item.folder + QLatin1Char('/') + item.name,
            categoryName(item.category), item.isDir ? QStringLiteral("Folder") : QStringLiteral("File")};
        for (int side = 0; side < result.sides; ++side)
            cells << item.path[side] << (item.size[side] < 0 ? QString() : QString::number(item.size[side]))
                  << (item.mtime[side].isValid() ? item.mtime[side].toUTC().toString(Qt::ISODate) : QString());
        if (!row(cells)) return fail(error, file.errorString());
    }
    if (!file.commit()) return fail(error, file.errorString());
    return true;
}
} // namespace lm
