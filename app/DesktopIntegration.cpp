// MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-06.
// SPDX-License-Identifier: GPL-3.0-or-later
// Based on LibreMerge's desktop integration by Iago Passos and contributors.
#include "DesktopIntegration.h"

#include <cstdio>
#include <cstring>

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QList>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace
{
const char kInstallFlag[] = "--install-desktop-integration";
const char kRemoveFlag[] = "--remove-desktop-integration";
const QString kMarkerKey = QStringLiteral("X-MergeDesk-Launcher");
const QByteArray kManagedMarker("MergeDesk managed integration v1");
const QString kThunarId = QStringLiteral("mergedesk-compare-v1");
const int kIconSizes[] = {16, 32, 64, 128, 256, 512};

bool fail(QString *error, const QString &text)
{
	if (error != nullptr)
		*error = text;
	return false;
}

QString configHome()
{
	const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
	return !xdg.isEmpty() && QDir::isAbsolutePath(xdg)
		? QDir::cleanPath(xdg) : QDir::homePath() + QStringLiteral("/.config");
}

QString desktopFilePath(const QString &dataHome)
{
	return dataHome + QStringLiteral("/applications/mergedesk.desktop");
}
QString iconPath(const QString &dataHome, int size)
{
	return dataHome + QStringLiteral("/icons/hicolor/%1x%1/apps/mergedesk.png").arg(size);
}
QString bundledIcon(int size)
{
	return QStringLiteral(":/desktop/icons/mergedesk-%1.png").arg(size);
}

// Desktop Entry values and Exec arguments each have an escaping layer.
// %% is essential: a literal percent in the executable is not a field code.
QString quoteExecArgument(const QString &path)
{
	QString escaped;
	for (const QChar c : path)
	{
		if (c == QLatin1Char('"') || c == QLatin1Char('`')
			|| c == QLatin1Char('$') || c == QLatin1Char('\\'))
			escaped += QLatin1Char('\\');
		escaped += c;
	}
	escaped.replace(QLatin1String("%"), QLatin1String("%%"));
	escaped.replace(QLatin1String("\\"), QLatin1String("\\\\"));
	return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString quoteShellArgument(QString value)
{
	value.replace(QLatin1String("'"), QLatin1String("'\\''"));
	value.replace(QLatin1String("%"), QLatin1String("%%"));
	return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QByteArray readAll(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool safePath(const QString &path, QString *error)
{
	if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path)
		return fail(error, QCoreApplication::translate("DesktopIntegration", "An absolute path is required: %1").arg(path));
	QString current = QDir::cleanPath(path);
	for (;;)
	{
		if (QFileInfo(current).isSymLink())
			return fail(error, QCoreApplication::translate("DesktopIntegration", "Symbolic links are not changed: %1").arg(current));
		const QString parent = QFileInfo(current).absolutePath();
		if (parent == current)
			break;
		current = parent;
	}
	if (QFileInfo(path).exists() && !QFileInfo(path).isFile())
		return fail(error, QCoreApplication::translate("DesktopIntegration", "The destination is not a regular file: %1").arg(path));
	return true;
}

bool checkOwnedFile(const QString &path, const QByteArray &marker, QString *error)
{
	if (!safePath(path, error))
		return false;
	if (QFileInfo::exists(path) && !readAll(path).contains(marker))
		return fail(error, QCoreApplication::translate("DesktopIntegration", "This file was not created by MergeDesk and was left unchanged: %1").arg(path));
	return true;
}

bool writeAtomic(const QString &path, const QByteArray &data,
	QStringList *written, QString *error)
{
	if (!safePath(path, error))
		return false;
	if (!QDir().mkpath(QFileInfo(path).absolutePath()))
		return fail(error, QCoreApplication::translate("DesktopIntegration", "Cannot create the folder for %1").arg(path));
	QSaveFile file(path);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()
		|| !file.commit())
		return fail(error, QCoreApplication::translate("DesktopIntegration", "Cannot write %1: %2").arg(path, file.errorString()));
	if (written != nullptr)
		written->append(path);
	return true;
}

bool writeOwned(const QString &path, const QByteArray &data,
	QStringList *written, QString *error)
{
	return checkOwnedFile(path, kManagedMarker, error)
		&& writeAtomic(path, data, written, error);
}

bool removeOwned(const QString &path, const QByteArray &marker,
	QStringList *removed, QString *error)
{
	if (!checkOwnedFile(path, marker, error))
		return false;
	if (!QFileInfo::exists(path))
		return true;
	if (!QFile::remove(path))
		return fail(error, QCoreApplication::translate("DesktopIntegration", "Cannot remove %1").arg(path));
	if (removed != nullptr)
		removed->append(path);
	return true;
}

bool validLauncher(const QString &launcher, QString *error)
{
	const QFileInfo file(launcher);
	if (!file.isAbsolute() || !file.isFile() || !file.isExecutable()
		|| launcher.contains(QLatin1Char('\n')) || launcher.contains(QLatin1Char('\r')))
		return fail(error, QCoreApplication::translate("DesktopIntegration", "The compiled MergeDesk executable was not found: %1").arg(launcher));
	return true;
}

void refreshCaches(const QString &dataHome)
{
	const QString updateDb = QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
	if (!updateDb.isEmpty())
		QProcess::execute(updateDb, {dataHome + QStringLiteral("/applications")});
	const QString iconCache = QStandardPaths::findExecutable(QStringLiteral("gtk-update-icon-cache"));
	const QString hicolor = dataHome + QStringLiteral("/icons/hicolor");
	if (!iconCache.isEmpty() && QFileInfo(hicolor).isDir())
		QProcess::execute(iconCache, {QStringLiteral("-f"), QStringLiteral("-t"), QStringLiteral("-q"), hicolor});
}

QString translatedEntryNames()
{
	return QStringLiteral("Name=Compare with MergeDesk\nName[hu]=Összehasonlítás a MergeDesk programmal\n"
		"Name[de]=Mit MergeDesk vergleichen\nName[fr]=Comparer avec MergeDesk\n");
}

QByteArray dolphinEntry(const QString &launcher, int count)
{
	return (QStringLiteral("# %1\n[Desktop Entry]\nType=Service\nMimeType=all/all;\n"
		"X-KDE-ServiceTypes=KonqPopupMenu/Plugin\nX-KDE-Protocol=file\n"
		"X-KDE-RequiredNumberOfUrls=%2\nActions=Compare;\n\n[Desktop Action Compare]\n")
		.arg(QString::fromUtf8(kManagedMarker)).arg(count) + translatedEntryNames()
		+ QStringLiteral("Icon=mergedesk\nExec=%1 %F\n").arg(quoteExecArgument(launcher))).toUtf8();
}

QByteArray nemoEntry(const QString &launcher, int count)
{
	return (QStringLiteral("# %1\n[Nemo Action]\n").arg(QString::fromUtf8(kManagedMarker))
		+ translatedEntryNames()
		+ QStringLiteral("Comment=Compare selected files or folders\nIcon-Name=mergedesk\n"
			"Exec=%1 %F\nSelection=%2\nExtensions=any;\nQuote=double\nEscapeSpaces=false\n")
			.arg(quoteExecArgument(launcher), QString::number(count))).toUtf8();
}

QByteArray nautilusExtension(const QString &launcher)
{
	// Literal hex avoids Python string injection for every valid filename.
	return QByteArray("# ") + kManagedMarker + QByteArray("\n"
		"# Copyright (C) 2026 davoda1. SPDX-License-Identifier: GPL-3.0-or-later\n"
		"# Requires the distribution's python3-nautilus package.\n"
		"import subprocess\nimport gi\n"
		"try:\n    gi.require_version('Nautilus', '4.0')\n"
		"except ValueError:\n    gi.require_version('Nautilus', '3.0')\n"
		"from gi.repository import GObject, Nautilus\n\n"
		"EXECUTABLE = bytes.fromhex('") + launcher.toUtf8().toHex() + QByteArray("').decode('utf-8')\nLABEL = bytes.fromhex('")
		+ QCoreApplication::translate("DesktopIntegration", "Compare with MergeDesk").toUtf8().toHex() + QByteArray("').decode('utf-8')\nTIP = bytes.fromhex('")
		+ QCoreApplication::translate("DesktopIntegration", "Compare selected files or folders").toUtf8().toHex() + QByteArray("').decode('utf-8')\n\n"
		"class MergeDeskMenuProvider(GObject.GObject, Nautilus.MenuProvider):\n"
		"    def get_file_items(self, *args):\n"
		"        files = args[-1]\n"
		"        if len(files) not in (2, 3):\n            return []\n"
		"        paths = [item.get_location().get_path() for item in files]\n"
		"        if not all(paths):\n            return []\n"
		"        item = Nautilus.MenuItem(name='MergeDesk::compare',\n"
		"            label=LABEL, tip=TIP)\n"
		"        item.connect('activate', self.compare, tuple(paths))\n"
		"        return [item]\n\n"
		"    def compare(self, item, paths):\n"
		"        try:\n            subprocess.Popen([EXECUTABLE, *paths], shell=False, close_fds=True)\n"
		"        except OSError as error:\n"
		"            # File-manager logging surfaces missing/moved executables.\n"
		"            print('MergeDesk:', error)\n");
}

// Copy one XML subtree; preserve unrelated actions, attributes and comments.
void copyElement(QXmlStreamReader &reader, QXmlStreamWriter &writer)
{
	writer.writeCurrentToken(reader);
	int depth = 1;
	while (depth > 0 && !reader.atEnd())
	{
		reader.readNext();
		if (reader.hasError()) break;
		if (reader.isStartElement()) ++depth;
		if (reader.isEndElement()) --depth;
		writer.writeCurrentToken(reader);
	}
}

bool updateThunar(const QString &path, const QString &launcher, bool install,
	QStringList *changed, QString *error)
{
	if (!safePath(path, error)) return false;
	const bool exists = QFileInfo::exists(path);
	if (!exists && !install) return true;
	const QByteArray original = exists ? readAll(path) : QByteArray("<actions/>");
	QXmlStreamReader reader(original);
	if (!reader.readNextStartElement() || reader.name() != QLatin1String("actions"))
		return fail(error, QCoreApplication::translate("DesktopIntegration", "The Thunar configuration is invalid and was left unchanged: %1").arg(path));
	QByteArray output;
	QXmlStreamWriter writer(&output);
	writer.setAutoFormatting(true);
	writer.writeStartDocument();
	writer.writeStartElement(QStringLiteral("actions"));
	writer.writeAttributes(reader.attributes());
	bool found = false;
	while (!reader.atEnd())
	{
		reader.readNext();
		if (reader.isEndElement() && reader.name() == QLatin1String("actions")) break;
		if (reader.isStartElement())
		{
			QByteArray fragment;
			QXmlStreamWriter fragmentWriter(&fragment);
			copyElement(reader, fragmentWriter);
			QXmlStreamReader inspect(fragment);
			QString id;
			while (inspect.readNextStartElement())
			{
				if (inspect.name() == QLatin1String("unique-id")) id = inspect.readElementText();
				else if (inspect.name() != QLatin1String("action")) inspect.skipCurrentElement();
			}
			if (id == kThunarId)
			{
				if (!fragment.contains(kManagedMarker))
					return fail(error, QCoreApplication::translate("DesktopIntegration", "The existing Thunar action was not created by MergeDesk and was left unchanged."));
				found = true;
				continue;
			}
			QXmlStreamReader restore(fragment);
			if (restore.readNextStartElement()) copyElement(restore, writer);
		}
		else if (reader.isComment()) writer.writeCurrentToken(reader);
	}
	// Read beyond the root too: malformed trailing data must not be replaced.
	while (!reader.atEnd()) reader.readNext();
	if (reader.hasError())
		return fail(error, QCoreApplication::translate("DesktopIntegration", "The Thunar configuration is invalid and was left unchanged: %1").arg(path));
	if (install)
	{
		writer.writeStartElement(QStringLiteral("action"));
		writer.writeComment(QString::fromUtf8(kManagedMarker));
		writer.writeTextElement(QStringLiteral("icon"), QStringLiteral("mergedesk"));
		writer.writeTextElement(QStringLiteral("name"), QCoreApplication::translate("DesktopIntegration", "Compare with MergeDesk"));
		writer.writeTextElement(QStringLiteral("unique-id"), kThunarId);
		writer.writeTextElement(QStringLiteral("command"), quoteShellArgument(launcher) + QStringLiteral(" %F"));
		writer.writeTextElement(QStringLiteral("description"), QCoreApplication::translate("DesktopIntegration", "Compare two or three selected files or folders"));
		writer.writeTextElement(QStringLiteral("patterns"), QStringLiteral("*"));
		writer.writeTextElement(QStringLiteral("range"), QStringLiteral("2-3"));
		for (const auto *category : {"directories", "audio-files", "image-files", "other-files", "text-files", "video-files"})
			writer.writeEmptyElement(QString::fromLatin1(category));
		writer.writeEndElement();
	}
	writer.writeEndElement();
	writer.writeEndDocument();
	if (!install && !found) return true;
	return writeAtomic(path, output, changed, error);
}

#ifdef Q_OS_WIN
bool explorerIntegration(const QString &launcher, bool install,
	QStringList *changed, QString *error)
{
	if (install && !validLauncher(launcher, error)) return false;
	const QString commandPrefix = QLatin1Char('"') + QDir::toNativeSeparators(launcher) + QStringLiteral("\" ");
	for (const QString &kind : {QStringLiteral("*"), QStringLiteral("Directory")})
	{
		for (const QString &verb : {QStringLiteral("Select"), QStringLiteral("Compare")})
		{
			const QString path = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1\\shell\\MergeDesk.%2").arg(kind, verb);
			QSettings settings(path, QSettings::NativeFormat);
			const bool exists = !settings.allKeys().isEmpty();
			if (exists && settings.value(QStringLiteral("MergeDeskManaged")).toString() != QString::fromUtf8(kManagedMarker))
				return fail(error, QCoreApplication::translate("DesktopIntegration", "An existing Explorer action was left unchanged: %1").arg(path));
			if (install)
			{
				const bool select = verb == QLatin1String("Select");
				settings.setValue(QStringLiteral("."), select ? QCoreApplication::translate("DesktopIntegration", "Select for MergeDesk comparison") : QCoreApplication::translate("DesktopIntegration", "Compare with selected in MergeDesk"));
				settings.setValue(QStringLiteral("Icon"), QDir::toNativeSeparators(launcher));
				settings.setValue(QStringLiteral("MultiSelectModel"), QStringLiteral("Single"));
				settings.setValue(QStringLiteral("MergeDeskManaged"), QString::fromUtf8(kManagedMarker));
				settings.setValue(QStringLiteral("command/."), commandPrefix + (select
					? QStringLiteral("--select-for-compare \"%1\"")
					: QStringLiteral("--compare-with-selected \"%1\"")));
			}
			else if (exists)
			{
				QSettings shell(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1\\shell").arg(kind), QSettings::NativeFormat);
				shell.remove(QStringLiteral("MergeDesk.%1").arg(verb));
				shell.sync();
				if (shell.status() != QSettings::NoError)
					return fail(error, QCoreApplication::translate("DesktopIntegration", "Cannot update the current user's Explorer integration: %1").arg(path));
			}
			settings.sync();
			if (settings.status() != QSettings::NoError)
				return fail(error, QCoreApplication::translate("DesktopIntegration", "Cannot update the current user's Explorer integration: %1").arg(path));
			if (changed != nullptr && (install || exists)) changed->append(path);
		}
	}
	return true;
}
#endif

// Exercises only an isolated temporary profile. It never installs anything in
// the caller's real home or registry and never executes a generated command.
int fileManagerSelfTest()
{
#ifdef Q_OS_LINUX
	QTemporaryDir temporary;
	if (!temporary.isValid()) return 2;
	const QString data = temporary.path() + QStringLiteral("/data");
	const QString config = temporary.path() + QStringLiteral("/config");
	const QString executable = temporary.path() + QStringLiteral("/Merge Desk $' %1.bin");
	QFile binary(executable);
	if (!binary.open(QIODevice::WriteOnly) || binary.write("test fixture") < 0) return 2;
	binary.close();
	QFile::setPermissions(executable, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
	const QString thunar = config + QStringLiteral("/Thunar/uca.xml");
	QString error;
	const QByteArray original("<?xml version=\"1.0\"?><actions><!-- keep me --><action><name>Existing</name><unique-id>existing-action</unique-id><command>example %F</command></action></actions>");
	if (!writeAtomic(thunar, original, nullptr, &error)) return 2;
	for (const QString &manager : {QStringLiteral("dolphin"), QStringLiteral("nautilus"), QStringLiteral("nemo"), QStringLiteral("thunar")})
	{
		if (!lm::installFileManagerIntegration(manager, executable, data, config, nullptr, &error)
			|| !lm::installFileManagerIntegration(manager, executable, data, config, nullptr, &error))
		{
			std::fprintf(stderr, "%s: %s\n", qPrintable(manager), qPrintable(error));
			return 1;
		}
	}
	const QByteArray thunarData = readAll(thunar);
	bool ok = thunarData.count(kThunarId.toUtf8()) == 1
		&& thunarData.contains("existing-action") && thunarData.contains("keep me")
		&& readAll(data + QStringLiteral("/kio/servicemenus/mergedesk-2.desktop")).contains("X-KDE-RequiredNumberOfUrls=2\n")
		&& readAll(data + QStringLiteral("/kio/servicemenus/mergedesk-3.desktop")).contains("X-KDE-RequiredNumberOfUrls=3\n")
		&& readAll(data + QStringLiteral("/nemo/actions/mergedesk-2.nemo_action")).contains("%%1.bin")
		&& readAll(data + QStringLiteral("/nautilus-python/extensions/mergedesk.py")).contains(executable.toUtf8().toHex());
	for (const QString &manager : {QStringLiteral("dolphin"), QStringLiteral("nautilus"), QStringLiteral("nemo"), QStringLiteral("thunar")})
		ok = lm::removeFileManagerIntegration(manager, data, config, nullptr, &error) && ok;
	ok = ok && readAll(thunar).contains("existing-action") && !readAll(thunar).contains(kThunarId.toUtf8());
	const QString foreign = data + QStringLiteral("/kio/servicemenus/mergedesk-2.desktop");
	const QByteArray foreignData("[Desktop Entry]\nName=My custom action\n");
	ok = writeAtomic(foreign, foreignData, nullptr, &error) && ok;
	ok = !lm::installFileManagerIntegration(QStringLiteral("dolphin"), executable, data, config, nullptr, &error)
		&& readAll(foreign) == foreignData && ok;
	ok = !lm::removeFileManagerIntegration(QStringLiteral("dolphin"), data, config, nullptr, &error)
		&& readAll(foreign) == foreignData && ok;
	const QByteArray malformed("<actions><action>");
	ok = writeAtomic(thunar, malformed, nullptr, &error) && ok;
	ok = !lm::installFileManagerIntegration(QStringLiteral("thunar"), executable, data, config, nullptr, &error)
		&& readAll(thunar) == malformed && ok;
	const QString actual = temporary.path() + QStringLiteral("/elsewhere");
	QDir().mkpath(actual);
	const QString linked = temporary.path() + QStringLiteral("/linked");
	ok = QFile::link(actual, linked) && ok;
	ok = !lm::installFileManagerIntegration(QStringLiteral("nautilus"), executable, linked, config, nullptr, &error)
		&& !QFileInfo::exists(actual + QStringLiteral("/nautilus-python")) && ok;
	std::printf("file-manager integration: isolated install/remove, quoting, idempotency, foreign entries, malformed XML and symlink guards: %s\n", ok ? "ok" : "FAILED");
	return ok ? 0 : 1;
#else
	std::fprintf(stderr, "This isolated file-manager test applies to GNU/Linux; Explorer integration is not changed by self-tests.\n");
	return 2;
#endif
}

} // namespace

namespace lm
{
QString desktopDataHome()
{
	const QString xdg = qEnvironmentVariable("XDG_DATA_HOME");
	return !xdg.isEmpty() && QDir::isAbsolutePath(xdg)
		? QDir::cleanPath(xdg) : QDir::homePath() + QStringLiteral("/.local/share");
}

QString desktopLauncher()
{
	const QString appImage = qEnvironmentVariable("APPIMAGE");
	if (!appImage.isEmpty() && QFileInfo(appImage).isFile())
		return QFileInfo(appImage).absoluteFilePath();
	return QCoreApplication::applicationFilePath();
}

bool installDesktopIntegration(const QString &dataHome, const QString &launcher,
	QStringList *written, QString *error)
{
	if (!QDir::isAbsolutePath(launcher) || launcher.contains(QLatin1Char('\n'))
		|| launcher.contains(QLatin1Char('\r')))
		return fail(error, QCoreApplication::translate("DesktopIntegration", "An absolute path is required: %1").arg(launcher));
	QString entry = QString::fromUtf8(readAll(QStringLiteral(":/desktop/mergedesk.desktop")));
	if (entry.isEmpty()) return fail(error, QCoreApplication::translate("DesktopIntegration", "The bundled desktop entry is missing."));
	const QString desktopFile = desktopFilePath(dataHome);
	if (!checkOwnedFile(desktopFile, kMarkerKey.toUtf8() + '=', error)) return false;
	for (const int size : kIconSizes)
	{
		const QString target = iconPath(dataHome, size);
		if (!safePath(target, error)) return false;
		if (QFileInfo::exists(target) && readAll(target) != readAll(bundledIcon(size)))
			return fail(error, QCoreApplication::translate("DesktopIntegration", "This file was not created by MergeDesk and was left unchanged: %1").arg(target));
	}
	QStringList lines;
	for (const QString &line : entry.split(QLatin1Char('\n')))
	{
		if (line.startsWith(QLatin1String("Exec=")))
		{
			lines.append(QStringLiteral("Exec=%1 %F").arg(quoteExecArgument(launcher)));
			lines.append(QStringLiteral("TryExec=%1").arg(launcher));
		}
		else if (!line.startsWith(QLatin1String("TryExec="))) lines.append(line);
	}
	while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
	lines.append(kMarkerKey + QLatin1Char('=') + launcher);
	if (!writeAtomic(desktopFile, (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8(), written, error)) return false;
	for (const int size : kIconSizes)
		if (!writeAtomic(iconPath(dataHome, size), readAll(bundledIcon(size)), written, error)) return false;
	refreshCaches(dataHome);
	return true;
}

bool removeDesktopIntegration(const QString &dataHome, QStringList *removed, QString *error)
{
	if (!removeOwned(desktopFilePath(dataHome), kMarkerKey.toUtf8() + '=', removed, error)) return false;
	for (const int size : kIconSizes)
	{
		const QString target = iconPath(dataHome, size);
		if (!safePath(target, error)) return false;
		if (QFileInfo::exists(target) && readAll(target) == readAll(bundledIcon(size)))
		{
			if (!QFile::remove(target)) return fail(error, QCoreApplication::translate("DesktopIntegration", "Cannot remove %1").arg(target));
			if (removed != nullptr) removed->append(target);
		}
	}
	refreshCaches(dataHome);
	return true;
}

bool installFileManagerIntegration(const QString &manager, const QString &launcher,
	const QString &dataHome, const QString &configurationHome, QStringList *written, QString *error)
{
	if (!validLauncher(launcher, error)) return false;
#ifdef Q_OS_WIN
	Q_UNUSED(dataHome); Q_UNUSED(configurationHome);
	if (manager == QLatin1String("explorer")) return explorerIntegration(launcher, true, written, error);
#else
	if (manager == QLatin1String("dolphin"))
	{
		for (const int count : {2, 3})
			if (!checkOwnedFile(dataHome + QStringLiteral("/kio/servicemenus/mergedesk-%1.desktop").arg(count), kManagedMarker, error)) return false;
		for (const int count : {2, 3})
		{
			const QString path = dataHome + QStringLiteral("/kio/servicemenus/mergedesk-%1.desktop").arg(count);
			if (!writeOwned(path, dolphinEntry(launcher, count), written, error)) return false;
			if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
				| QFileDevice::ReadGroup | QFileDevice::ReadOther))
				return fail(error, QCoreApplication::translate("DesktopIntegration", "Cannot set permissions on %1").arg(path));
		}
		return true;
	}
	if (manager == QLatin1String("nautilus"))
		return writeOwned(dataHome + QStringLiteral("/nautilus-python/extensions/mergedesk.py"), nautilusExtension(launcher), written, error);
	if (manager == QLatin1String("nemo"))
	{
		for (const int count : {2, 3})
		{
			const QString path = dataHome + QStringLiteral("/nemo/actions/mergedesk-%1.nemo_action").arg(count);
			if (!checkOwnedFile(path, kManagedMarker, error)) return false;
		}
		for (const int count : {2, 3})
			if (!writeOwned(dataHome + QStringLiteral("/nemo/actions/mergedesk-%1.nemo_action").arg(count), nemoEntry(launcher, count), written, error)) return false;
		return true;
	}
	if (manager == QLatin1String("thunar"))
		return updateThunar(configurationHome + QStringLiteral("/Thunar/uca.xml"), launcher, true, written, error);
#endif
	return fail(error, QCoreApplication::translate("DesktopIntegration", "This file manager is not supported on this operating system: %1").arg(manager));
}

bool removeFileManagerIntegration(const QString &manager, const QString &dataHome,
	const QString &configurationHome, QStringList *removed, QString *error)
{
#ifdef Q_OS_WIN
	Q_UNUSED(dataHome); Q_UNUSED(configurationHome);
	if (manager == QLatin1String("explorer")) return explorerIntegration(QString(), false, removed, error);
#else
	if (manager == QLatin1String("dolphin"))
	{
		for (const int count : {2, 3})
			if (!removeOwned(dataHome + QStringLiteral("/kio/servicemenus/mergedesk-%1.desktop").arg(count), kManagedMarker, removed, error)) return false;
		return true;
	}
	if (manager == QLatin1String("nautilus"))
		return removeOwned(dataHome + QStringLiteral("/nautilus-python/extensions/mergedesk.py"), kManagedMarker, removed, error);
	if (manager == QLatin1String("nemo"))
	{
		for (const int count : {2, 3})
			if (!removeOwned(dataHome + QStringLiteral("/nemo/actions/mergedesk-%1.nemo_action").arg(count), kManagedMarker, removed, error)) return false;
		return true;
	}
	if (manager == QLatin1String("thunar"))
		return updateThunar(configurationHome + QStringLiteral("/Thunar/uca.xml"), QString(), false, removed, error);
#endif
	return fail(error, QCoreApplication::translate("DesktopIntegration", "This file manager is not supported on this operating system: %1").arg(manager));
}

void showDesktopIntegrationDialog(QWidget *parent)
{
	QDialog dialog(parent);
	dialog.setObjectName(QStringLiteral("desktopIntegrationDialog"));
	dialog.setWindowTitle(QCoreApplication::translate("DesktopIntegration", "Desktop Integration"));
	dialog.setWindowModality(Qt::WindowModal);
	dialog.resize(590, 510);
	auto *layout = new QVBoxLayout(&dialog);
	auto *intro = new QLabel(QCoreApplication::translate("DesktopIntegration", "Add optional shortcuts for the current user. The shortcuts start the compiled MergeDesk executable at the location below. Reinstall them after moving the application."), &dialog);
	intro->setWordWrap(true);
	layout->addWidget(intro);
	auto *location = new QLabel(QDir::toNativeSeparators(desktopLauncher()), &dialog);
	location->setWordWrap(true);
	location->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(location);
	QList<QPair<QCheckBox *, QString>> choices;
#ifdef Q_OS_WIN
	choices.append({new QCheckBox(QCoreApplication::translate("DesktopIntegration", "Windows Explorer context menu"), &dialog), QStringLiteral("explorer")});
	auto *hint = new QLabel(QCoreApplication::translate("DesktopIntegration", "Select one file or folder with 'Select for MergeDesk comparison', then use 'Compare with selected in MergeDesk' on another. Windows 11 may show these actions under 'Show more options'."), &dialog);
#else
	choices.append({new QCheckBox(QCoreApplication::translate("DesktopIntegration", "Applications menu and icon"), &dialog), QStringLiteral("applications")});
	choices.append({new QCheckBox(QStringLiteral("KDE Dolphin"), &dialog), QStringLiteral("dolphin")});
	choices.append({new QCheckBox(QStringLiteral("GNOME Files (Nautilus)"), &dialog), QStringLiteral("nautilus")});
	choices.append({new QCheckBox(QStringLiteral("Cinnamon Nemo"), &dialog), QStringLiteral("nemo")});
	choices.append({new QCheckBox(QStringLiteral("Xfce Thunar"), &dialog), QStringLiteral("thunar")});
	auto *hint = new QLabel(QCoreApplication::translate("DesktopIntegration", "Select two or three files or folders in the file manager. Nautilus needs the distribution's python3-nautilus package. Close Thunar before changing its actions. Restart the file manager after installation or removal."), &dialog);
#endif
	for (const auto &choice : choices)
	{
		choice.first->setObjectName(QStringLiteral("integration_") + choice.second);
		layout->addWidget(choice.first);
	}
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *result = new QPlainTextEdit(&dialog);
	result->setObjectName(QStringLiteral("integrationResult"));
	result->setReadOnly(true);
	result->setMinimumHeight(100);
	layout->addWidget(result, 1);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	auto *install = buttons->addButton(QCoreApplication::translate("DesktopIntegration", "Install Selected"), QDialogButtonBox::ActionRole);
	auto *remove = buttons->addButton(QCoreApplication::translate("DesktopIntegration", "Remove Selected"), QDialogButtonBox::ActionRole);
	install->setObjectName(QStringLiteral("installIntegrationButton"));
	remove->setObjectName(QStringLiteral("removeIntegrationButton"));
	layout->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	auto apply = [&](bool installing) {
		QStringList output;
		bool any = false;
		for (const auto &choice : choices)
		{
			if (!choice.first->isChecked()) continue;
			any = true;
			QStringList paths;
			QString error;
			bool ok;
			if (choice.second == QLatin1String("applications"))
				ok = installing ? installDesktopIntegration(desktopDataHome(), desktopLauncher(), &paths, &error)
					: removeDesktopIntegration(desktopDataHome(), &paths, &error);
			else
				ok = installing ? installFileManagerIntegration(choice.second, desktopLauncher(), desktopDataHome(), configHome(), &paths, &error)
					: removeFileManagerIntegration(choice.second, desktopDataHome(), configHome(), &paths, &error);
			output.append(choice.first->text() + QStringLiteral(": ") + (ok
				? (installing ? QCoreApplication::translate("DesktopIntegration", "Installed") : QCoreApplication::translate("DesktopIntegration", "Removed")) : error));
			output.append(paths);
		}
		if (!any) output.append(QCoreApplication::translate("DesktopIntegration", "Select at least one integration."));
		result->setPlainText(output.join(QLatin1Char('\n')));
	};
	QObject::connect(install, &QPushButton::clicked, &dialog, [&] {apply(true);});
	QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {apply(false);});
	dialog.exec();
}

int runDesktopIntegrationCommand(int argc, char *argv[])
{
	bool relevant = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], kInstallFlag) == 0 || std::strcmp(argv[i], kRemoveFlag) == 0
			|| std::strncmp(argv[i], "--install-file-manager-integration=", 35) == 0
			|| std::strncmp(argv[i], "--remove-file-manager-integration=", 34) == 0
			|| std::strcmp(argv[i], "--select-for-compare") == 0
			|| std::strcmp(argv[i], "--compare-with-selected") == 0
			|| std::strcmp(argv[i], "--selftest-file-manager-integration") == 0) relevant = true;
	if (!relevant) return -1;
	QCoreApplication app(argc, argv);
	const QStringList args = app.arguments();
	if (args.contains(QStringLiteral("--selftest-file-manager-integration"))) return fileManagerSelfTest();
	const bool installApp = args.contains(QString::fromLatin1(kInstallFlag));
	const bool removeApp = args.contains(QString::fromLatin1(kRemoveFlag));
	QString installManager, removeManager;
	QString dataHome = desktopDataHome(), configurationHome = configHome();
	for (const QString &arg : args)
	{
		if (arg.startsWith(QLatin1String("--install-file-manager-integration="))) installManager = arg.mid(35);
		if (arg.startsWith(QLatin1String("--remove-file-manager-integration="))) removeManager = arg.mid(34);
		if (arg.startsWith(QLatin1String("--data-home="))) dataHome = QDir::cleanPath(QDir::current().absoluteFilePath(arg.mid(12)));
		if (arg.startsWith(QLatin1String("--config-home="))) configurationHome = QDir::cleanPath(QDir::current().absoluteFilePath(arg.mid(14)));
	}
	const int selectIndex = args.indexOf(QStringLiteral("--select-for-compare"));
	const int compareIndex = args.indexOf(QStringLiteral("--compare-with-selected"));
	const int operations = int(installApp) + int(removeApp) + int(!installManager.isEmpty())
		+ int(!removeManager.isEmpty()) + int(selectIndex >= 0) + int(compareIndex >= 0);
	if (operations != 1)
	{
		std::fprintf(stderr, "MergeDesk: specify exactly one integration operation\n");
		return 2;
	}
	QStringList paths;
	QString error;
	bool ok = false;
	if (selectIndex >= 0 || compareIndex >= 0)
	{
		const int index = selectIndex >= 0 ? selectIndex : compareIndex;
		if (index + 1 >= args.size() || !QFileInfo::exists(args.at(index + 1)))
		{
			std::fprintf(stderr, "MergeDesk: an existing file or folder must follow the selection flag\n");
			return 2;
		}
		QSettings selected(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
			+ QStringLiteral("/MergeDesk/shell.ini"), QSettings::IniFormat);
		const QString path = QFileInfo(args.at(index + 1)).absoluteFilePath();
		if (selectIndex >= 0)
		{
			selected.setValue(QStringLiteral("SelectedPath"), path);
			selected.sync();
			ok = selected.status() == QSettings::NoError;
			if (!ok) error = QCoreApplication::translate("DesktopIntegration", "Cannot save the selected comparison path.");
		}
		else
		{
			const QString left = selected.value(QStringLiteral("SelectedPath")).toString();
			if (left.isEmpty() || !QFileInfo::exists(left))
			{
				// Opening the native selection screen is useful even when the prior
				// selection was deleted. Preserve the newly selected path.
				ok = QProcess::startDetached(desktopLauncher(), {path});
			}
			else ok = QProcess::startDetached(desktopLauncher(), {left, path});
			if (!ok) error = QCoreApplication::translate("DesktopIntegration", "Cannot start MergeDesk.");
		}
	}
	else if (!installManager.isEmpty())
		ok = installFileManagerIntegration(installManager, desktopLauncher(), dataHome, configurationHome, &paths, &error);
	else if (!removeManager.isEmpty())
		ok = removeFileManagerIntegration(removeManager, dataHome, configurationHome, &paths, &error);
	else
	{
#ifdef Q_OS_LINUX
		ok = installApp ? installDesktopIntegration(dataHome, desktopLauncher(), &paths, &error)
			: removeDesktopIntegration(dataHome, &paths, &error);
#else
		error = QCoreApplication::translate("DesktopIntegration", "Application-menu integration is available on GNU/Linux.");
#endif
	}
	for (const QString &path : paths) std::printf("%s\n", qPrintable(path));
	if (!ok) std::fprintf(stderr, "MergeDesk: %s\n", qPrintable(error));
	return ok ? 0 : 1;
}
} // namespace lm
