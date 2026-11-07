// MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-06.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

class QWidget;

/**
 * Applications-menu integration for the Linux AppImage: a user-level
 * .desktop entry pointing at the AppImage file plus the hicolor icons,
 * the job AppImageLauncher or a distribution package would otherwise do.
 * Used by `--install-desktop-integration` / `--remove-desktop-integration`
 * (also what the Homebrew cask runs after installing on Linux).
 */
namespace lm
{

/** $XDG_DATA_HOME, or ~/.local/share. */
QString desktopDataHome();

/** The file the menu entry launches: the AppImage ($APPIMAGE) when
    running from one, else this executable. */
QString desktopLauncher();

/** Write <dataHome>/applications/mergedesk.desktop and the icons. */
bool installDesktopIntegration(const QString &dataHome, const QString &launcher,
	QStringList *written, QString *error);

/** Remove what installDesktopIntegration wrote; a mergedesk.desktop
    without MergeDesk's marker (made by hand, or by a package) stays. */
bool removeDesktopIntegration(const QString &dataHome, QStringList *removed,
	QString *error);

/** User-level context-menu integrations. Manager is one of dolphin,
    nautilus, nemo, thunar, explorer. Explicit data/config homes make Linux
    operations testable without writing to a real profile. */
bool installFileManagerIntegration(const QString &manager, const QString &launcher,
    const QString &dataHome, const QString &configHome, QStringList *written,
    QString *error);
bool removeFileManagerIntegration(const QString &manager, const QString &dataHome,
    const QString &configHome, QStringList *removed, QString *error);

/** An optional, user-initiated dialog. No integration is installed by
    merely opening the program or opening this dialog. */
void showDesktopIntegrationDialog(QWidget *parent = nullptr);

/** Handle the integration and Explorer selection flags before the GUI starts, so they work
    without a display. Returns the exit code, or -1 when neither flag is
    present. */
int runDesktopIntegrationCommand(int argc, char *argv[]);

} // namespace lm
