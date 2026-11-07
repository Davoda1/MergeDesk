// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace lm {
/** Focused, deterministic integration tests for projects, exports and native
 * document conversion. Requires an existing QCoreApplication. Returns 0 on
 * success, 1 on failure; writes the check counts to standard output. */
int runFeatureTests();
}
