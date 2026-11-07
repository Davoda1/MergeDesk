// MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-06.
// Copyright (C) 2026 davoda1 — MergeDesk About interface.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "AboutDialog.h"
#include "ProjectLinks.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QSysInfo>
#include <QTextBrowser>
#include <QVBoxLayout>

AboutDialog::AboutDialog(QWidget *parent):QDialog(parent)
{
    setWindowTitle(tr("About MergeDesk"));
    setMinimumWidth(520);
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(24,22,24,20);layout->setSpacing(14);
    auto *header=new QHBoxLayout;
    auto *icon=new QLabel(this);icon->setPixmap(QIcon(QStringLiteral(":/about/mergedesk.svg")).pixmap(82,82));header->addWidget(icon);
    auto *identity=new QVBoxLayout;
    auto *name=new QLabel(QStringLiteral("MergeDesk"),this);QFont font=name->font();font.setPointSize(26);font.setBold(true);name->setFont(font);
    identity->addWidget(name);identity->addWidget(new QLabel(versionText()+QStringLiteral("  ·  ")+platformText(),this));
    identity->addWidget(new QLabel(tr("File and folder comparison, wherever you work."),this));
    header->addLayout(identity,1);layout->addLayout(header);
    auto *author=new QLabel(QStringLiteral("Copyright (C) 2026 davoda1"),this);author->setObjectName(QStringLiteral("mergeDeskCopyright"));
    QFont credit=author->font();credit.setBold(true);author->setFont(credit);layout->addWidget(author);
    auto *description=new QLabel(tr("Native Qt6 comparison and merging for Windows and GNU/Linux. Based on WinMerge's comparison engines and the LibreMerge Qt interface by Iago Passos."),this);
    description->setWordWrap(true);description->setMaximumWidth(560);layout->addWidget(description);
    auto *baseline=new QLabel(tr("Compatibility reference: WinMerge 2.16.60. Upstream authors retain their copyrights; MergeDesk is an independent project."),this);
    baseline->setWordWrap(true);layout->addWidget(baseline);
    auto *license=new QLabel(tr("GNU General Public License, version 3 or later. This software comes with absolutely no warranty."),this);license->setWordWrap(true);layout->addWidget(license);
    auto *link=new QLabel(QStringLiteral("<a href=\"%1\">%2</a> · <a href=\"https://winmerge.org/\">WinMerge</a>").arg(homepageUrl().toString(),tr("MergeDesk project")),this);link->setOpenExternalLinks(true);layout->addWidget(link);
    auto *buttons=new QHBoxLayout;
    auto *notices=new QPushButton(tr("Third-party notices"),this);connect(notices,&QPushButton::clicked,this,&AboutDialog::showContributors);buttons->addWidget(notices);
    auto *licenseButton=new QPushButton(tr("License"),this);buttons->addWidget(licenseButton);
    connect(licenseButton,&QPushButton::clicked,this,[this]{
        QDialog dialog(this);dialog.setWindowTitle(tr("GNU GPL version 3"));dialog.resize(720,560);
        auto *box=new QVBoxLayout(&dialog);auto *text=new QTextBrowser(&dialog);QFile file(QStringLiteral(":/about/LICENSE"));if(file.open(QIODevice::ReadOnly))text->setPlainText(QString::fromUtf8(file.readAll()));box->addWidget(text);
        auto *close=new QDialogButtonBox(QDialogButtonBox::Close,&dialog);box->addWidget(close);connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);dialog.exec();
    });
    buttons->addStretch();auto *ok=new QPushButton(tr("OK"),this);ok->setDefault(true);buttons->addWidget(ok);connect(ok,&QPushButton::clicked,this,&QDialog::accept);layout->addLayout(buttons);
    layout->setSizeConstraint(QLayout::SetFixedSize);
}
QString AboutDialog::versionText(){return tr("Version %1 (%2)").arg(QApplication::applicationVersion(),QStringLiteral("2026.11"));}
QString AboutDialog::platformText(){
#ifdef Q_OS_WIN
    return QStringLiteral("Windows ")+QSysInfo::buildCpuArchitecture();
#else
    return QStringLiteral("GNU/Linux ")+QSysInfo::buildCpuArchitecture();
#endif
}
QUrl AboutDialog::homepageUrl(){return lm::projectPage();}
QString AboutDialog::contributorsMarkdown(){QFile file(QStringLiteral(":/about/CONTRIBUTORS.md"));return file.open(QIODevice::ReadOnly)?QString::fromUtf8(file.readAll()):QString();}
void AboutDialog::showContributors(){
    QDialog dialog(this);dialog.setWindowTitle(tr("Third-party notices"));dialog.resize(720,580);
    auto *layout=new QVBoxLayout(&dialog);auto *browser=new QTextBrowser(&dialog);browser->setOpenExternalLinks(true);browser->setMarkdown(contributorsMarkdown());browser->document()->setDocumentMargin(14);layout->addWidget(browser);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);dialog.exec();
}
