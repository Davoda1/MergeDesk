// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 davoda1
#include "pch.h"
#include "locality.h"
#include <QLocale>
#include <QDateTime>

namespace locality {
String GetLocaleStr(const tchar_t* input, int decimals) {
    const QString text = QString::fromUtf8(input);
    bool ok = false;
    if (decimals > 0) {
        const double number = text.toDouble(&ok);
        if (ok) return QLocale().toString(number, 'f', decimals).toUtf8().toStdString();
    } else {
        const qlonglong number = text.toLongLong(&ok);
        if (ok) return QLocale().toString(number).toUtf8().toStdString();
    }
    return input;
}
String NumToLocaleStr(int value) { return QLocale().toString(value).toUtf8().toStdString(); }
String NumToLocaleStr(int64_t value) { return QLocale().toString(qlonglong(value)).toUtf8().toStdString(); }
String TimeString(const int64_t* time) {
    if (!time) return "---";
    if (*time == INT64_MIN / 1000 / 1000) return {};
    const QDateTime date = QDateTime::fromSecsSinceEpoch(*time);
    return date.isValid() ? QLocale().toString(date, QLocale::ShortFormat).toUtf8().toStdString() : String("---");
}
bool ParseDateTime(const String& text, int64_t& result) {
    const QString input = QString::fromUtf8(text.data(), qsizetype(text.size()));
    QDateTime date = QDateTime::fromString(input, Qt::ISODate);
    if (!date.isValid()) date = QLocale().toDateTime(input, QLocale::ShortFormat);
    if (!date.isValid()) {
        const QStringList formats{"yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd", "MM/dd/yyyy HH:mm:ss", "MM/dd/yyyy", "dd.MM.yyyy HH:mm:ss", "dd.MM.yyyy"};
        for (const auto& format : formats) { date = QDateTime::fromString(input, format); if (date.isValid()) break; }
    }
    if (!date.isValid()) {
        QTime time = QTime::fromString(input, "HH:mm:ss");
        if (time.isValid()) date = QDateTime(QDate(1970,1,1), time);
    }
    if (!date.isValid()) return false;
    result = date.toMSecsSinceEpoch() * 1000;
    return true;
}
} // namespace locality
