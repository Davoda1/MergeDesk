// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>
namespace md {
struct AutoMergeResult { QByteArray text; int conflicts=0; QString error; };
AutoMergeResult mergeText(const QByteArray &base,const QByteArray &left,const QByteArray &right);
}
