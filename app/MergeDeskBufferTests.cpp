// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MergeDeskBufferTests.h"
#include "FileCompareView.h"
#include "TableCompareView.h"
#include "EngineOptions.h"
#include "OptionsDef.h"
#include "OptionsMgr.h"
#include <QAction>
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextStream>

namespace md {
namespace {
struct Checks {
    int passed = 0, failed = 0;
    void expect(bool condition, const QString &name, const QString &detail = {}) {
        if (condition) { ++passed; return; }
        ++failed;
        QTextStream(stderr) << "FAIL: " << name
            << (detail.isEmpty() ? QString() : QStringLiteral(": ") + detail) << '\n';
    }
    int finish(const char *name) const {
        QTextStream(stdout) << name << ": " << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    }
};
bool put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray get(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
// Mixed input files should not trigger a question in a noninteractive test.
// This affects comparison sensitivity, not the bytes exported or saved.
struct ComparisonOptions {
    COptionsMgr *options = GetOptionsMgr();
    bool ignoreEol = options && options->GetBool(OPT_CMP_IGNORE_EOL);
    bool autoRescan = options && options->GetBool(OPT_AUTOMATIC_RESCAN);
    ComparisonOptions() {
        if (options) {
            options->Set(OPT_CMP_IGNORE_EOL, true);
            options->Set(OPT_AUTOMATIC_RESCAN, false);
        }
    }
    ~ComparisonOptions() {
        if (options) {
            options->Set(OPT_CMP_IGNORE_EOL, ignoreEol);
            options->Set(OPT_AUTOMATIC_RESCAN, autoRescan);
        }
    }
};
QByteArray utf16le(const QString &text) {
    QByteArray bytes = QByteArray::fromHex("fffe");
    for (const QChar ch : text) {
        bytes.append(char(ch.unicode() & 255));
        bytes.append(char(ch.unicode() >> 8));
    }
    return bytes;
}
}

int runBufferTests()
{
    Checks check;
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    ComparisonOptions options;
    struct Case { const char *name; QByteArray before, after; };
    const Case cases[] = {
        {"ordinary final LF", "one\nlast\n", "ONE\nLAST\n"},
        {"add final LF", "one\nlast", "one\nlast\n"},
        {"remove final CRLF", "one\r\nlast\r\n", "one\r\nlast"},
        {"mixed endings", "one\r\nsecond\nthird\rlast", "ONE\nsecond\r\nthird\rlast\r\n"},
        {"mixed to CR", "one\nsecond\rthird\r\n", "ONE\rSECOND\r"},
        {"empty to one newline", {}, "\n"},
        {"one newline to empty", "\n", {}},
        {"empty to CRLF", {}, "\r\n"},
        {"blank lines and missing final ending", "\n\n", "\r\n\n\rtail"},
        {"ending-only change", "same\n", "same\r\n"},
        {"leading real blank over ghosts", "anchor\nend\n", "\nanchor\r\nnew\nend"},
        {"multiple final newlines", "line", "line\n\n\n"},
    };
    int sequence = 0;
    for (const Case &item : cases) {
        const QString name = QString::fromLatin1(item.name);
        const QString prefix = directory.filePath(QString::number(sequence++));
        const QString left = prefix + QStringLiteral("-left.txt");
        const QString base = prefix + QStringLiteral("-base.txt");
        const QString right = prefix + QStringLiteral("-right.txt");
        const QByteArray leftBytes = QByteArray("left-only\n") + item.before + "\nleft-tail\n";
        const QByteArray rightBytes = item.before + "\nright-only\nright-tail\n";
        check.expect(put(left, leftBytes) && put(base, item.before) && put(right, rightBytes), name + QStringLiteral(" fixture"));
        QString error;
        FileCompareView view;
        const bool opened = view.compare({left, base, right}, &error);
        check.expect(opened, name + QStringLiteral(" opens"), error);
        if (!opened) continue;
        check.expect(view.utf8Snapshot(1) == item.before, name + QStringLiteral(" original snapshot"));
        view.replaceContents(1, QString::fromUtf8(item.after));
        check.expect(view.utf8Snapshot(1) == item.after && view.isModified(), name + QStringLiteral(" replacement is exact"),
            QString::fromLatin1(view.utf8Snapshot(1).toHex()));
        check.expect(view.utf8Snapshot(0) == leftBytes && view.utf8Snapshot(2) == rightBytes,
            name + QStringLiteral(" other panes unchanged"));
        check.expect(get(base) == item.before, name + QStringLiteral(" review before save"));
        view.undoActive();
        check.expect(view.utf8Snapshot(1) == item.before && !view.isModified(), name + QStringLiteral(" one undo restores text and metadata"),
            QString::fromLatin1(view.utf8Snapshot(1).toHex()));
        view.redoActive();
        check.expect(view.utf8Snapshot(1) == item.after && view.isModified(), name + QStringLiteral(" one redo restores result"));
        check.expect(view.saveSideAt(1, &error) && get(base) == item.after && !view.isModified(), name + QStringLiteral(" save is byte exact"), error);
        view.undoActive();
        check.expect(view.utf8Snapshot(1) == item.before && view.isModified() && get(base) == item.after,
            name + QStringLiteral(" undo after save stays unsaved"));
        view.redoActive();
        check.expect(view.utf8Snapshot(1) == item.after && !view.isModified(),
            name + QStringLiteral(" redo to saved result is clean"));
    }

    const QString left = directory.filePath(QStringLiteral("history-left.txt"));
    const QString base = directory.filePath(QStringLiteral("history-base.txt"));
    const QString right = directory.filePath(QStringLiteral("history-right.txt"));
    const QByteArray original("original\r\nlast");
    put(left, "left\n"); put(base, original); put(right, "right\n");
    QString error;
    {
        FileCompareView view;
        check.expect(view.compare({left, base, right}, &error), QStringLiteral("history fixture opens"), error);
        const QByteArray first("FIRST\nlast\r\n");
        view.replaceContents(1, QString::fromUtf8(first));
        view.replaceContents(1, QString());
        view.undoActive();
        check.expect(view.utf8Snapshot(1) == first, QStringLiteral("second replacement undoes independently"));
        view.undoActive();
        check.expect(view.utf8Snapshot(1) == original, QStringLiteral("first replacement remains undoable"));
        view.replaceContents(1, QString::fromUtf8(original));
        view.redoActive();
        check.expect(view.utf8Snapshot(1) == first, QStringLiteral("identical replacement keeps redo history"));
        view.redoActive();
        check.expect(view.utf8Snapshot(1).isEmpty(), QStringLiteral("second replacement remains redoable"));
    }
    {
        FileCompareView view;
        check.expect(view.compare({left, base, right}, &error), QStringLiteral("persistent conversion fixture opens"), error);
        view.replaceContents(1, QStringLiteral("FIRST\nlast\n"));
        view.convertLineEndings(1, QStringLiteral("\r"));
        view.undoActive();
        check.expect(view.utf8Snapshot(1) == "original\rlast", QStringLiteral("undo respects later explicit EOL conversion"));
        view.redoActive();
        check.expect(view.utf8Snapshot(1) == "FIRST\rlast\r", QStringLiteral("redo respects later explicit EOL conversion"));
    }
    {
        FileCompareView view;
        view.setReadOnlySides({false, true, false});
        check.expect(view.compare({left, base, right}, &error), QStringLiteral("protected text fixture opens"), error);
        view.replaceContents(1, QStringLiteral("forbidden\n"));
        check.expect(view.utf8Snapshot(1) == original && !view.isModified() && get(base) == original,
            QStringLiteral("read-only text rejects automatic replacement"));
        view.convertLineEndings(1, QStringLiteral("\n"));
        check.expect(view.utf8Snapshot(1) == original && !view.isModified(),
            QStringLiteral("read-only text rejects EOL conversion"));
        QMenu menu;
        view.buildConvertEolMenu(&menu, 1);
        bool allDisabled = menu.actions().size() == 3;
        for (const QAction *action : menu.actions())
            allDisabled = allDisabled && !action->isEnabled();
        check.expect(allDisabled, QStringLiteral("all protected text EOL commands are disabled"));
        const QStringList beforeFiles = QDir(directory.path()).entryList(QDir::Files);
        error.clear();
        check.expect(!view.saveSideAt(1, &error) && !error.isEmpty() && get(base) == original
            && QDir(directory.path()).entryList(QDir::Files) == beforeFiles,
            QStringLiteral("protected explicit save fails before writing or creating a backup"), error);
    }
    {
        FileCompareView view;
        check.expect(view.compare({left, base, right}, &error), QStringLiteral("dirty text protection fixture opens"), error);
        view.replaceContents(1, QStringLiteral("unsaved edit\n"));
        view.setReadOnlySides({false, true, false});
        const QStringList beforeFiles = QDir(directory.path()).entryList(QDir::Files);
        error.clear();
        check.expect(!view.saveModified(&error) && !error.isEmpty() && get(base) == original
            && view.isModified() && view.utf8Snapshot(1) == "unsaved edit\n"
            && QDir(directory.path()).entryList(QDir::Files) == beforeFiles,
            QStringLiteral("locking dirty text rejects save and preserves pending changes"), error);
    }
    {
        const QString before = QString::fromUtf8("árvíz\r\nmás");
        const QString after = QString::fromUtf8("őriz\r\nmás\nvég\r");
        put(base, utf16le(before));
        FileCompareView view;
        check.expect(view.compare({left, base, right}, &error), QStringLiteral("UTF-16 merge target opens"), error);
        check.expect(view.utf8Snapshot(1) == before.toUtf8(), QStringLiteral("UTF-16 snapshot is decoded UTF-8 without BOM"));
        view.replaceContents(1, after);
        view.undoActive();
        check.expect(view.utf8Snapshot(1) == before.toUtf8(), QStringLiteral("UTF-16 replacement undoes exactly"));
        view.redoActive();
        check.expect(view.saveSideAt(1, &error) && get(base) == utf16le(after),
            QStringLiteral("replacement preserves UTF-16 encoding, BOM and mixed EOLs"), error);
    }
    return check.finish("MergeDesk buffer replacement");
}

int runTableReadOnlyTests()
{
    Checks check;
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    ComparisonOptions options;
    const QString left = directory.filePath(QStringLiteral("left.csv"));
    const QString middle = directory.filePath(QStringLiteral("middle.csv"));
    const QString right = directory.filePath(QStringLiteral("right.csv"));
    const QByteArray leftBytes("id,value\n1,left\n2,common\n");
    const QByteArray middleBytes("id,value\n1,middle\n2,common\n");
    const QByteArray rightBytes("id,value\n1,right\n2,common\n");
    const auto resetFiles = [&] {
        return put(left, leftBytes) && put(middle, middleBytes) && put(right, rightBytes);
    };
    QString error;
    check.expect(resetFiles(), QStringLiteral("table protection fixtures"));
    {
        TableCompareView view;
        view.setReadOnlySides({false, true});
        const bool opened = view.compare({left, right}, &error);
        check.expect(opened, QStringLiteral("protected table opens"), error);
        if (!opened) return check.finish("MergeDesk read-only tables");
        check.expect(view.isSideReadOnly(1) && !view.isSideReadOnly(0), QStringLiteral("protection survives compare"));
        QAbstractItemModel *model = view.tableForTest(1)->model();
        const QModelIndex index = model->index(0, 1);
        check.expect(index.isValid() && !(model->flags(index) & Qt::ItemIsEditable)
            && view.tableForTest(1)->editTriggers() == QAbstractItemView::NoEditTriggers,
            QStringLiteral("native table refuses editing a protected cell"));
        check.expect(!model->setData(index, QStringLiteral("forbidden"), Qt::EditRole),
            QStringLiteral("model rejects protected setData"));
        view.editCell(1, 0, 1, QStringLiteral("forbidden"));
        view.gotoFirstDiff();
        view.copyCurrentDiff(0, 1);
        view.copyAllFrom(0, 1);
        view.selectCellsForTest(0, 0, 0, 1, 1);
        view.copyLines(0, 1);
        view.copyCurrentDiff(0, 1);
        view.convertLineEndings(1, QStringLiteral("\r\n"));
        QCoreApplication::processEvents();
        check.expect(view.utf8Snapshot(1) == rightBytes && !view.isModified(),
            QStringLiteral("edit, hunk, all, selected cells, lines and EOL changes are denied"));
        QMenu menu;
        view.buildCellMenu(0, &menu);
        bool guardedActions = true;
        for (const QAction *action : menu.actions())
            if (action->objectName() == QLatin1String("cellCopy_0_1")
                || action->objectName() == QLatin1String("cellCopyLines_0_1"))
                guardedActions = guardedActions && !action->isEnabled();
        check.expect(guardedActions, QStringLiteral("protected merge actions are disabled"));
        check.expect(view.saveModified(&error) && get(right) == rightBytes,
            QStringLiteral("saving unchanged protected table does not write"), error);
        view.selectCellsForTest(1, 0, 0, 0, 0);
        view.gotoFirstDiff();
        view.copyCurrentDiff(1, 0);
        check.expect(view.utf8Snapshot(0) == rightBytes && view.utf8Snapshot(1) == rightBytes && view.isModified(),
            QStringLiteral("protected side remains usable as a merge source"));
        view.undo();
        check.expect(view.utf8Snapshot(0) == leftBytes, QStringLiteral("allowed table merge remains undoable"));
    }
    {
        TableCompareView view;
        check.expect(view.compare({left, right}, &error), QStringLiteral("deferred edit fixture opens"), error);
        auto *model = view.tableForTest(1)->model();
        check.expect(model->setData(model->index(0, 1), QStringLiteral("queued edit"), Qt::EditRole),
            QStringLiteral("writable model queues its edit"));
        view.setReadOnlySides({false, true});
        QCoreApplication::processEvents();
        check.expect(view.utf8Snapshot(1) == rightBytes && !view.isModified(),
            QStringLiteral("protection blocks an already queued edit"));
    }
    {
        TableCompareView view;
        check.expect(view.compare({left, right}, &error), QStringLiteral("save preflight fixture opens"), error);
        view.editCell(0, 0, 1, QStringLiteral("left edit"));
        view.editCell(1, 0, 1, QStringLiteral("right edit"));
        const QByteArray dirtyRight = view.utf8Snapshot(1);
        view.setReadOnlySides({false, true});
        check.expect(!view.saveModified(&error) && !error.isEmpty()
            && get(left) == leftBytes && get(right) == rightBytes,
            QStringLiteral("locked dirty pane rejects save before any other write"));
        view.undo();
        check.expect(view.utf8Snapshot(1) == dirtyRight, QStringLiteral("undo cannot modify a protected pane"));
        view.setReadOnlySides({false, false});
        view.undo();
        check.expect(view.utf8Snapshot(1) == rightBytes, QStringLiteral("unlock restores access to undo history"));
        view.setReadOnlySides({false, true});
        view.redo();
        check.expect(view.utf8Snapshot(1) == rightBytes, QStringLiteral("redo cannot modify a protected pane"));
    }
    {
        TableCompareView view;
        view.setReadOnlySides({true, false});
        check.expect(view.compare({left, right}, &error), QStringLiteral("swap protection fixture opens"), error);
        view.swapSides();
        check.expect(view.paths() == QStringList{right, left} && !view.isSideReadOnly(0) && view.isSideReadOnly(1),
            QStringLiteral("protection follows its file when sides swap"));
        view.editCell(1, 0, 1, QStringLiteral("forbidden"));
        check.expect(view.utf8Snapshot(1) == leftBytes && !view.isModified(), QStringLiteral("swapped protected file stays unchanged"));
        check.expect(view.reload(&error) && view.isSideReadOnly(1), QStringLiteral("protection survives reload"), error);
    }
    {
        TableCompareView view;
        view.setReadOnlySides({true, false, true});
        check.expect(view.compare({left, middle, right}, &error), QStringLiteral("three-way protection fixture opens"), error);
        view.copyAllFrom(1, 0); view.copyAllFrom(1, 2);
        view.editCell(0, 0, 1, QStringLiteral("forbidden")); view.editCell(2, 0, 1, QStringLiteral("forbidden"));
        check.expect(view.utf8Snapshot(0) == leftBytes && view.utf8Snapshot(2) == rightBytes && !view.isModified(),
            QStringLiteral("both three-way source panes are protected"));
        view.copyAllFrom(0, 1);
        check.expect(view.utf8Snapshot(1) == leftBytes && view.utf8Snapshot(0) == leftBytes
            && view.utf8Snapshot(2) == rightBytes, QStringLiteral("three-way merge into writable middle succeeds"));
        check.expect(view.saveModified(&error) && get(middle) == leftBytes && get(left) == leftBytes && get(right) == rightBytes,
            QStringLiteral("three-way save writes only the writable destination"), error);
    }
    return check.finish("MergeDesk read-only tables");
}
}
