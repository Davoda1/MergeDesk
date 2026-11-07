// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MergeDeskEncodingTests.h"
#include "FileCompareView.h"
#include "TableCompareView.h"
#include "DiffTextEdit.h"
#include "EngineOptions.h"
#include "OptionsDef.h"
#include "OptionsMgr.h"
#include "unicoder.h"
#include <QAction>
#include <QAbstractItemModel>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QTemporaryDir>
#include <QTableView>
#include <QTextBlock>
#include <QTextStream>
#include <cstdio>

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
    int finish() const {
        QTextStream(stdout) << "MergeDesk output encoding: " << passed << " passed, "
            << failed << " failed\n";
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
struct OptionsGuard {
    COptionsMgr *options = GetOptionsMgr();
    bool ignoreEol = options && options->GetBool(OPT_CMP_IGNORE_EOL);
    bool ignoreCodepage = options && options->GetBool(OPT_CMP_IGNORE_CODEPAGE);
    bool autoRescan = options && options->GetBool(OPT_AUTOMATIC_RESCAN);
    int detection = options ? options->GetInt(OPT_CP_DETECT) : 0;
    int codepage = ucr::getDefaultCodepage();
    bool mixed = lm::allowMixedEol();
    OptionsGuard() {
        if (options) {
            options->Set(OPT_CMP_IGNORE_EOL, true);
            options->Set(OPT_CMP_IGNORE_CODEPAGE, false);
            options->Set(OPT_AUTOMATIC_RESCAN, false);
            // Keep valid UTF-8 recognition, but disable statistical guessing
            // so the non-UTF-8 fixture deterministically uses Windows-1252.
            options->Set(OPT_CP_DETECT, 1);
        }
        lm::setAllowMixedEolForTest(true);
        lm::updateDefaultCodepage(2, 1252);
    }
    ~OptionsGuard() {
        if (options) {
            options->Set(OPT_CMP_IGNORE_EOL, ignoreEol);
            options->Set(OPT_CMP_IGNORE_CODEPAGE, ignoreCodepage);
            options->Set(OPT_AUTOMATIC_RESCAN, autoRescan);
            options->Set(OPT_CP_DETECT, detection);
        }
        lm::setAllowMixedEolForTest(mixed);
        lm::updateDefaultCodepage(2, codepage);
        DiffTextEdit::setContextMenuPresenterForTest({});
    }
};
using Encoding = FileCompareView::OutputEncoding;
QByteArray encoded(const QString &text, Encoding encoding) {
    if (encoding == Encoding::Utf8)
        return text.toUtf8();
    if (encoding == Encoding::Utf8Bom)
        return QByteArray::fromHex("efbbbf") + text.toUtf8();
    const bool little = encoding == Encoding::Utf16LE;
    QByteArray bytes = QByteArray::fromHex(little ? "fffe" : "feff");
    // Independent expected-byte construction, including surrogate pairs.
    for (const QChar ch : text) {
        bytes.append(char(little ? ch.unicode() & 255 : ch.unicode() >> 8));
        bytes.append(char(little ? ch.unicode() >> 8 : ch.unicode() & 255));
    }
    return bytes;
}
QString checkedEncoding(FileCompareView &view, int side) {
    QMenu menu;
    view.buildOutputEncodingMenu(&menu, side);
    QString checked;
    int count = 0;
    for (const QAction *action : menu.actions())
        if (action->isChecked()) { checked = action->objectName(); ++count; }
    return count == 1 ? checked : QString();
}
QList<bool> ghostFlags(const DiffTextEdit *pane) {
    QList<bool> flags;
    for (QTextBlock block = pane->document()->begin(); block.isValid(); block = block.next())
        flags.append(block.userData() != nullptr);
    return flags;
}
}

int runEncodingTests()
{
    Checks check;
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    OptionsGuard options;
    const QByteArray original("caf\xe9\r\nsecond\nthird\rtail");
    const QString originalText = QString::fromLatin1(original);
    const QString prefix = QString::fromUtf8("😀 ");
    const QString editedText = prefix + originalText;
    struct Case { Encoding encoding; const char *action; };
    const Case cases[] = {
        { Encoding::Utf8, "encodingUtf8" }, { Encoding::Utf8Bom, "encodingUtf8Bom" },
        { Encoding::Utf16LE, "encodingUtf16LE" }, { Encoding::Utf16BE, "encodingUtf16BE" },
    };
    int sequence = 0;
    QString error;
    for (const Case &item : cases) {
        const QString name = QString::fromLatin1(item.action);
        const QString left = directory.filePath(QString::number(sequence) + QStringLiteral("-left.txt"));
        const QString right = directory.filePath(QString::number(sequence++) + QStringLiteral("-right.txt"));
        check.expect(put(left, original) && put(right, originalText.toUtf8()), name + QStringLiteral(" fixtures"));
        const QDateTime originalTime = QFileInfo(left).lastModified();
        const QStringList originalFiles = QDir(directory.path()).entryList(QDir::Files);
        FileCompareView view;
        const bool opened = view.compare(left, right, &error);
        check.expect(opened, name + QStringLiteral(" opens legacy input"), error);
        if (!opened) continue;
        const QString legacyLabel = view.encodingTextForTest(0);
        check.expect(view.utf8Snapshot(0) == originalText.toUtf8()
            && legacyLabel.contains(QStringLiteral("1252")), name + QStringLiteral(" decodes Windows-1252"),
            QStringLiteral("label=%1; snapshot=%2; expected=%3; default=%4; detection=%5")
                .arg(legacyLabel, QString::fromLatin1(view.utf8Snapshot(0).toHex()),
                    QString::fromLatin1(originalText.toUtf8().toHex()))
                .arg(ucr::getDefaultCodepage()).arg(lm::codepageDetection()));
        view.typeAtForTest(0, 0, prefix);
        check.expect(view.utf8Snapshot(0) == editedText.toUtf8(), name + QStringLiteral(" editor accepts non-BMP text"));
        const auto panes = view.findChildren<DiffTextEdit *>();
        const QString alignedBefore = panes.at(0)->toPlainText();
        const QList<bool> ghostsBefore = ghostFlags(panes.at(0));
        const bool staleBefore = view.diffStaleForTest();
        check.expect(view.setOutputEncoding(0, item.encoding) && view.isModified()
            && checkedEncoding(view, 0) == name, name + QStringLiteral(" sets output and selected menu item"));
        check.expect(view.utf8Snapshot(0) == editedText.toUtf8() && view.mixedEolForTest(0)
            && panes.at(0)->toPlainText() == alignedBefore && ghostFlags(panes.at(0)) == ghostsBefore
            && view.diffStaleForTest() == staleBefore,
            name + QStringLiteral(" leaves text, ghosts, mixed endings and stale state intact"));
        check.expect(get(left) == original && QFileInfo(left).lastModified() == originalTime
            && QDir(directory.path()).entryList(QDir::Files) == originalFiles,
            name + QStringLiteral(" makes no file or backup writes before explicit Save"));
        view.undoActive();
        check.expect(view.encodingTextForTest(0) == legacyLabel && view.utf8Snapshot(0) == editedText.toUtf8()
            && view.isModified(), name + QStringLiteral(" one undo restores legacy encoding without undoing text"));
        view.undoActive();
        check.expect(view.utf8Snapshot(0) == originalText.toUtf8() && !view.isModified(),
            name + QStringLiteral(" earlier text edit still undoes to the clean original"));
        view.redoActive();
        view.redoActive();
        check.expect(view.utf8Snapshot(0) == editedText.toUtf8() && checkedEncoding(view, 0) == name
            && view.isModified(), name + QStringLiteral(" text and encoding redo in order"));
        const QByteArray expected = encoded(editedText, item.encoding);
        check.expect(view.saveSideAt(0, &error) && get(left) == expected && !view.isModified(),
            name + QStringLiteral(" Save writes exact BOM, Unicode bytes and mixed final-EOL state"), error);
        check.expect(get(right) == originalText.toUtf8(), name + QStringLiteral(" other input remains unchanged"));
        view.undoActive();
        check.expect(view.encodingTextForTest(0) == legacyLabel && view.isModified()
            && view.utf8Snapshot(0) == editedText.toUtf8() && get(left) == expected,
            name + QStringLiteral(" undo after Save changes metadata only and remains unsaved"));
        view.redoActive();
        check.expect(checkedEncoding(view, 0) == name && !view.isModified() && get(left) == expected,
            name + QStringLiteral(" redo returns to the clean saved encoding"));
        view.undoActive();
        view.undoActive();
        check.expect(view.saveSideAt(0, &error) && get(left) == original,
            name + QStringLiteral(" restored legacy codepage saves original bytes"), error);
    }

    const QString left = directory.filePath(QStringLiteral("menu-left.txt"));
    const QString right = directory.filePath(QStringLiteral("menu-right.txt"));
    const QByteArray utf8 = QString::fromUtf8("café\r\nlast").toUtf8();
    put(left, utf8); put(right, utf8);
    {
        FileCompareView view;
        check.expect(view.compare(left, right, &error), QStringLiteral("menu fixture opens"), error);
        auto *label = view.findChild<QLabel *>(QStringLiteral("encodingLabel0"));
        check.expect(label && label->focusPolicy() == Qt::StrongFocus && !label->accessibleName().isEmpty()
            && !label->toolTip().isEmpty(), QStringLiteral("encoding status is keyboard-focusable and described"));
        bool presented = false;
        DiffTextEdit::setContextMenuPresenterForTest([&](QMenu *menu) {
            presented = menu->actions().size() == 4;
            if (QAction *action = menu->findChild<QAction *>(QStringLiteral("encodingUtf16BE")))
                action->trigger();
        });
        if (label) {
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QApplication::sendEvent(label, &key);
        }
        DiffTextEdit::setContextMenuPresenterForTest({});
        check.expect(presented && checkedEncoding(view, 0) == QStringLiteral("encodingUtf16BE")
            && view.isModified() && get(left) == utf8, QStringLiteral("Space opens the native menu and defers output until Save"));
        check.expect(view.diffCount() == 0 && view.encodingsDiffer()
            && view.statusTextForTest().contains(FileCompareView::tr("Text is identical; encodings differ")),
            QStringLiteral("encoding-only difference updates status and comparison state"));
        view.undoActive();
        check.expect(!view.isModified() && !view.encodingsDiffer()
            && checkedEncoding(view, 0) == QStringLiteral("encodingUtf8"),
            QStringLiteral("encoding-only undo restores the original clean state"),
            QStringLiteral("label=%1; checked=%2; modified=%3; encodingsDiffer=%4")
                .arg(view.encodingTextForTest(0), checkedEncoding(view, 0))
                .arg(view.isModified()).arg(view.encodingsDiffer()));
        check.expect(view.setOutputEncoding(0, Encoding::Utf8) && !view.isModified(),
            QStringLiteral("selecting the current encoding is a clean no-op"));
        view.redoActive();
        check.expect(checkedEncoding(view, 0) == QStringLiteral("encodingUtf16BE") && view.isModified(),
            QStringLiteral("no-op selection preserves the encoding redo command"));
        check.expect(!view.setOutputEncoding(-1, Encoding::Utf8)
            && !view.setOutputEncoding(3, Encoding::Utf8)
            && !view.setOutputEncoding(0, static_cast<Encoding>(-1)),
            QStringLiteral("invalid pane and encoding selections are rejected"));
    }
    {
        FileCompareView view;
        view.setReadOnlySides({true, false});
        check.expect(view.compare(left, right, &error), QStringLiteral("protected fixture opens"), error);
        const QString before = view.encodingTextForTest(0);
        QMenu menu;
        view.buildOutputEncodingMenu(&menu, 0);
        bool disabled = menu.actions().size() == 4;
        for (QAction *action : menu.actions()) {
            disabled = disabled && !action->isEnabled();
            action->trigger();
        }
        check.expect(disabled && !view.setOutputEncoding(0, Encoding::Utf16LE)
            && view.encodingTextForTest(0) == before && !view.isModified() && get(left) == utf8,
            QStringLiteral("read-only panes reject menu and programmatic conversion"));
    }
    {
        put(left, original); put(right, originalText.toUtf8());
        FileCompareView view;
        check.expect(view.compare(left, right, &error), QStringLiteral("failed-save recovery fixture opens"), error);
        view.typeAtForTest(0, 0, prefix);
        error.clear();
        check.expect(!view.saveSideAt(0, &error) && !error.isEmpty() && get(left) == original,
            QStringLiteral("legacy Save refuses unrepresentable Unicode without replacing the input"), error);
        check.expect(view.setOutputEncoding(0, Encoding::Utf8Bom) && view.saveSideAt(0, &error)
            && get(left) == encoded(editedText, Encoding::Utf8Bom),
            QStringLiteral("Unicode output selection makes the formerly rejected Save succeed"), error);
    }
    {
        FileCompareView view;
        view.startBlank();
        check.expect(view.setOutputEncoding(1, Encoding::Utf16LE) && view.isModified()
            && view.utf8Snapshot(1).isEmpty(), QStringLiteral("empty untitled buffer can choose an output encoding"));
        view.undoActive();
        check.expect(!view.isModified() && checkedEncoding(view, 1) == QStringLiteral("encodingUtf8"),
            QStringLiteral("empty-buffer encoding change is undoable"));
    }
    {
        const QString middle = directory.filePath(QStringLiteral("ghost-middle.txt"));
        const QByteArray middleBytes("caf\xe9\nsecond\n");
        put(left, QString::fromUtf8("leading\ncafé\nsecond\n").toUtf8());
        put(middle, middleBytes);
        put(right, QString::fromUtf8("café\nsecond\ntrailing\n").toUtf8());
        FileCompareView view;
        check.expect(view.compare({left, middle, right}, &error),
            QStringLiteral("three-pane ghost fixture opens"), error);
        const auto panes = view.findChildren<DiffTextEdit *>();
        const QList<bool> initialGhosts = ghostFlags(panes.at(1));
        const QString alignedText = panes.at(1)->toPlainText();
        const QByteArray snapshot = view.utf8Snapshot(1);
        const QString originalEncoding = view.encodingTextForTest(1);
        check.expect(initialGhosts.contains(true), QStringLiteral("three-pane target contains real alignment ghosts"));
        view.setOutputEncoding(1, Encoding::Utf16LE);
        view.setOutputEncoding(1, Encoding::Utf16BE);
        check.expect(ghostFlags(panes.at(1)) == initialGhosts && panes.at(1)->toPlainText() == alignedText
            && view.utf8Snapshot(1) == snapshot && get(middle) == middleBytes,
            QStringLiteral("successive encoding changes preserve ghost lines and all source bytes"));
        view.undoActive();
        check.expect(checkedEncoding(view, 1) == QStringLiteral("encodingUtf16LE")
            && ghostFlags(panes.at(1)) == initialGhosts && view.utf8Snapshot(1) == snapshot,
            QStringLiteral("successive encoding choices undo independently over ghosts"));
        view.undoActive();
        check.expect(view.encodingTextForTest(1) == originalEncoding && !view.isModified()
            && ghostFlags(panes.at(1)) == initialGhosts && get(middle) == middleBytes,
            QStringLiteral("ghost-pane undo restores the original clean legacy encoding"));
        view.redoActive(); view.redoActive();
        check.expect(checkedEncoding(view, 1) == QStringLiteral("encodingUtf16BE")
            && ghostFlags(panes.at(1)) == initialGhosts && view.utf8Snapshot(1) == snapshot,
            QStringLiteral("successive encoding choices redo without phantom text"));
    }
    struct TableCase { const char *name; QByteArray bytes, changed; int rows, column; QString value, original; };
    const TableCase tables[] = {
        { "legacy CSV final row", "id,name\r\n1,caf\xe9\r\n2,last", "id,name\r\n1,caf\xe9\r\n2,changed",
            3, 1, QStringLiteral("changed"), QStringLiteral("last") },
        { "legacy multiline CSV final field", "id,name\r\n1,\"caf\xe9\r\nlast\"", "id,name\r\n9,\"caf\xe9\r\nlast\"",
            2, 0, QStringLiteral("9"), QStringLiteral("1") },
    };
    for (const TableCase &item : tables) {
        const QString name = QString::fromLatin1(item.name);
        const QString csvLeft = directory.filePath(QString::number(sequence) + QStringLiteral("-left.csv"));
        const QString csvRight = directory.filePath(QString::number(sequence++) + QStringLiteral("-right.csv"));
        check.expect(put(csvLeft, item.bytes) && put(csvRight, QString::fromLatin1(item.bytes).toUtf8()),
            name + QStringLiteral(" fixtures"));
        TableCompareView view;
        view.setTableOverrides(QChar(','), QChar('"'), true);
        error.clear();
        const bool opened = view.compare(csvLeft, csvRight, &error);
        check.expect(opened, name + QStringLiteral(" opens"), error);
        if (!opened) continue;
        check.expect(view.rawLinesForTest(0).size() == item.rows
            && view.utf8Snapshot(0) == QString::fromLatin1(item.bytes).toUtf8(),
            name + QStringLiteral(" retains unterminated final data without a phantom row"),
            QString::fromLatin1(view.utf8Snapshot(0).toHex()));
        // The CSV header can be a separate header widget, not model row 0.
        QAbstractItemModel *model = view.tableForTest(0)->model();
        int editRow = -1;
        for (int row = 0; row < model->rowCount(); ++row)
            if (model->index(row, item.column).data().toString() == item.original) {
                editRow = row;
                break;
            }
        check.expect(editRow >= 0, name + QStringLiteral(" final data is exposed as an editable cell"));
        view.editCell(0, editRow, item.column, item.value);
        check.expect(view.isModified() && get(csvLeft) == item.bytes,
            name + QStringLiteral(" editing does not write before Save"));
        check.expect(view.saveModified(&error) && get(csvLeft) == item.changed,
            name + QStringLiteral(" Save retains legacy bytes and no final newline"), error);
        view.editCell(0, editRow, item.column, item.original);
        check.expect(view.saveModified(&error) && get(csvLeft) == item.bytes,
            name + QStringLiteral(" explicit edit roundtrip restores byte-exact original"), error);
        check.expect(get(csvRight) == QString::fromLatin1(item.bytes).toUtf8(),
            name + QStringLiteral(" other input stays unchanged"));
    }
    return check.finish();
}
}
