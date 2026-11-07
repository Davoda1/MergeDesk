// MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-07.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FileOps.h"
#include "SafeFileOps.h"
#include "OptionsDialog.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>

namespace lm
{

bool copyRecursively(const QString &src, const QString &dst)
{
    const QFileInfo source(src), target(dst);
    if (source.isSymLink() || source.isJunction() || target.isSymLink() || target.isJunction()) return false;
    if (source.isDir()) {
        const QString sourceRoot = source.canonicalFilePath();
        const QString destination = QDir::cleanPath(target.absoluteFilePath());
#ifdef Q_OS_WIN
        constexpr auto pathCase = Qt::CaseInsensitive;
#else
        constexpr auto pathCase = Qt::CaseSensitive;
#endif
        if (destination.compare(sourceRoot, pathCase) == 0
            || destination.startsWith(sourceRoot + QLatin1Char('/'), pathCase))
            return false;
        QFileInfo parent(target.absolutePath());
        while (true) {
            if (parent.isSymLink() || parent.isJunction()) return false;
            // Resolve existing ancestors as well, so filesystem aliases
            // cannot hide a destination inside the source directory.
            if (parent.canonicalFilePath().compare(sourceRoot, pathCase) == 0) return false;
            const QString next = parent.absolutePath();
            if (next == parent.absoluteFilePath()) break;
            parent.setFile(next);
        }
        if (!QDir().mkpath(dst)) return false;
        const auto entries=QDir(src).entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System);
        for (const auto &entry:entries)
            if (!copyRecursively(entry.filePath(), QDir(dst).filePath(entry.fileName()))) return false;
        return true;
    }
    QString error;
    return md::copyFileAtomic(src,dst,true,&error);
}

QDateTime timeToPreserve(const QString &path)
{
	const QFileInfo info(path);
	return OptionsDialog::preserveFileTime() && info.exists()
		? info.lastModified() : QDateTime();
}

void restoreFileTime(const QString &path, const QDateTime &time)
{
	if (!time.isValid())
		return;
	// like CMergeDoc::DoSave, a failure here does not fail the save
	QFile file(path);
	if (file.open(QIODevice::Append))
		file.setFileTime(time, QFileDevice::FileModificationTime);
}

FileStamp fileStamp(const QString &path)
{
	FileStamp stamp;
	const QFileInfo info(path);
	if (!path.isEmpty() && info.exists())
	{
		stamp.modified = info.lastModified();
		stamp.size = info.size();
	}
	return stamp;
}

FileChange fileChangedOnDisk(const QString &path, const FileStamp &stamp)
{
	// "we assume file existed, so disappearing means removal"
	const QFileInfo info(path);
	if (path.isEmpty() || !info.exists())
		return FileChange::Removed;
	// any time difference counts: WinMerge's two-second tolerance comes
	// with OPT_IGNORE_SMALL_FILETIME, off by default and not offered here
	if (info.lastModified() != stamp.modified || info.size() != stamp.size)
		return FileChange::Changed;
	return FileChange::NoChange;
}

namespace
{
std::function<QString(const QString &)> s_saveAsAnswer;
}

QString askSaveAsPath(QWidget *parent, const QString &title, const QString &start,
	const QString &filter)
{
	if (s_saveAsAnswer)
		return s_saveAsAnswer(start);
	return QFileDialog::getSaveFileName(parent, title, start, filter);
}

void setSaveAsPathForTest(std::function<QString(const QString &)> answer)
{
	s_saveAsAnswer = std::move(answer);
}

} // namespace lm
