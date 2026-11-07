// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SafeFileOps.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDateTime>
#include <QUuid>
#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>
#include <limits>
#include <memory>

namespace md {
static bool fail(QString *error, const QString &message) { if (error) *error=message; return false; }

bool safeDestination(const QString &path, QString *error)
{
    if (path.isEmpty()) return fail(error,QCoreApplication::translate("SafeFileOps", "The destination path is empty."));
    QFileInfo entry(QFileInfo(path).absoluteFilePath());
    if (entry.exists() && !entry.isFile())
        return fail(error,QCoreApplication::translate("SafeFileOps", "The destination is not a regular file: %1").arg(path));
    // Do not write through links or linked parent directories. This also catches
    // dangling links, for which QFileInfo::exists() returns false.
    while (true) {
        if (entry.isSymLink() || entry.isJunction())
            return fail(error,QCoreApplication::translate("SafeFileOps", "Writing through a symbolic link is not allowed: %1").arg(entry.filePath()));
        const QString parent=entry.absolutePath();
        if (parent==entry.absoluteFilePath()) break;
        entry.setFile(parent);
    }
    return true;
}

bool createBackup(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path)) return true;
    if (!safeDestination(path,error)) return false;
    const QString target=path+QStringLiteral(".bak");
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) {
        if (!safeDestination(target,error)) return false;
        const QString previous=target+QStringLiteral(".")+QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"))
            +QStringLiteral("-")+QUuid::createUuid().toString(QUuid::Id128).left(8);
        if (!QFile::copy(target,previous)) return fail(error,QCoreApplication::translate("SafeFileOps", "Could not retain the previous backup of %1.").arg(path));
    }
    if (!copyFileAtomic(path,target,false,error)) return false;
    return true;
}

bool writeBytesAtomic(const QString &path, const QByteArray &bytes, QString *error)
{
    if (!safeDestination(path,error)) return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return fail(error,file.errorString());
    if (file.write(bytes)!=bytes.size()) { file.cancelWriting(); return fail(error,file.errorString()); }
    if (!file.commit()) return fail(error,file.errorString());
    return true;
}

bool copyFileAtomic(const QString &source,const QString &target,bool backup,QString *error)
{
    const QFileInfo src(source);
    if (!src.isFile() || src.isSymLink())
        return fail(error,QCoreApplication::translate("SafeFileOps", "Only regular files can be copied: %1").arg(source));
    if (src.canonicalFilePath()==QFileInfo(target).canonicalFilePath())
        return fail(error,QCoreApplication::translate("SafeFileOps", "The source and destination refer to the same file."));
    if (!safeDestination(target,error)) return false;
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return fail(error,QCoreApplication::translate("SafeFileOps", "Could not create the destination directory."));
    if (backup && !createBackup(target,error)) return false;
    QFile input(source);
    QSaveFile output(target);
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly)) return fail(error,input.errorString());
    if (!output.open(QIODevice::WriteOnly)) return fail(error,output.errorString());
    while (!input.atEnd()) {
        const QByteArray block=input.read(1024*1024);
        if (input.error()!=QFileDevice::NoError || output.write(block)!=block.size()) {
            output.cancelWriting();
            return fail(error,QCoreApplication::translate("SafeFileOps", "The copy failed; the previous destination was kept."));
        }
    }
    output.setPermissions(src.permissions());
    if (!output.commit()) return fail(error,output.errorString());
    return true;
}

static UConverter *converterFor(int unicodeSet,int codepage,UErrorCode &status)
{
    const char *unicodeName=nullptr;
    switch(unicodeSet) {
    case 1: unicodeName="UTF-16LE"; break;
    case 2: unicodeName="UTF-16BE"; break;
    case 3: unicodeName="UTF-8"; break;
    case 4: unicodeName="UTF-32LE"; break;
    case 5: unicodeName="UTF-32BE"; break;
    default: break;
    }
    if(unicodeName) return ucnv_open(unicodeName,&status);
    if(codepage==0 || codepage==65001) return ucnv_open("UTF-8",&status);
    if(codepage==1200) return ucnv_open("UTF-16LE",&status);
    if(codepage==1201) return ucnv_open("UTF-16BE",&status);
    if(codepage==20127) return ucnv_open("US-ASCII",&status);
    if(codepage>=28591 && codepage<=28606)
        return ucnv_open(("ISO-8859-"+QByteArray::number(codepage-28590)).constData(),&status);
    UConverter *c=ucnv_open(("windows-"+QByteArray::number(codepage)).constData(),&status);
    if(U_FAILURE(status)) { status=U_ZERO_ERROR; c=ucnv_open(("cp"+QByteArray::number(codepage)).constData(),&status); }
    return c;
}

bool writeTextAtomic(const QString &path,const QStringList &lines,const QStringList &endings,
    bool finalEol,int unicodeSet,int codepage,bool bom,QString *error)
{
    if(!safeDestination(path,error)) return false;
    UErrorCode status=U_ZERO_ERROR;
    std::unique_ptr<UConverter,decltype(&ucnv_close)> converter(converterFor(unicodeSet,codepage,status),ucnv_close);
    if(U_FAILURE(status) || !converter)
        return fail(error,QCoreApplication::translate("SafeFileOps", "The selected text encoding is not supported (%1).").arg(codepage));
    ucnv_setFromUCallBack(converter.get(),UCNV_FROM_U_CALLBACK_STOP,nullptr,nullptr,nullptr,&status);
    if(U_FAILURE(status)) return fail(error,QString::fromLatin1(u_errorName(status)));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)) return fail(error,file.errorString());
    QByteArray prefix;
    if(bom) {
        switch(unicodeSet) {
        case 1: prefix=QByteArray::fromHex("fffe"); break;
        case 2: prefix=QByteArray::fromHex("feff"); break;
        case 3: prefix=QByteArray::fromHex("efbbbf"); break;
        case 4: prefix=QByteArray::fromHex("fffe0000"); break;
        case 5: prefix=QByteArray::fromHex("0000feff"); break;
        default: break;
        }
    }
    if(file.write(prefix)!=prefix.size()) { file.cancelWriting();return fail(error,file.errorString()); }
    for(qsizetype i=0;i<lines.size();++i) {
        QString text=lines.at(i);
        if(i+1<lines.size() || finalEol) text+=endings.value(i,QStringLiteral("\n"));
        if(text.size()>std::numeric_limits<int32_t>::max()) {
            file.cancelWriting();return fail(error,QCoreApplication::translate("SafeFileOps", "This line exceeds the encoder's supported size."));
        }
        status=U_ZERO_ERROR;
        const int32_t needed=ucnv_fromUChars(converter.get(),nullptr,0,
            reinterpret_cast<const UChar*>(text.utf16()),static_cast<int32_t>(text.size()),&status);
        if(status!=U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
            file.cancelWriting();
            return fail(error,QCoreApplication::translate("SafeFileOps", "The original encoding cannot represent all characters. The file was not changed. Save in a Unicode encoding."));
        }
        status=U_ZERO_ERROR;
        QByteArray bytes(needed,'\0');
        ucnv_fromUChars(converter.get(),bytes.data(),needed,
            reinterpret_cast<const UChar*>(text.utf16()),static_cast<int32_t>(text.size()),&status);
        if(U_FAILURE(status) || file.write(bytes)!=bytes.size()) {
            file.cancelWriting();return fail(error,QCoreApplication::translate("SafeFileOps", "The write failed; the previous file was kept."));
        }
    }
    if(!file.commit()) return fail(error,file.errorString());
    return true;
}
}
