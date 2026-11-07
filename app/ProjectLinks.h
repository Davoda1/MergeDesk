// MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-06.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <QUrl>

/**
 * The project's pages on GitHub, which the Help menu, the About box and
 * documentation lead to. These explicit links open in the OS browser.
 * Only the optional webpage comparison loads sites inside the app.
 */
namespace lm
{

/** The repository, where the project is read about and starred. */
QUrl projectPage();
/** Where a problem is reported. */
QUrl projectIssuesPage();
/** Where native speakers are asked to review the translations. */
QUrl projectTranslationsPage();

/** Open a page in the browser. */
void openProjectPage(const QUrl &page);
/** What is handed the pages in the browser's place; an empty one gives
    them back to the browser (for tests). */
void setProjectPageOpenerForTest(std::function<void(const QUrl &page)> opener);

} // namespace lm
