// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"
#include "WebCompareView.h"
#include "FileCompareView.h"
#include "TableCompareView.h"
#include "ImageCompareView.h"
#include "HexCompareView.h"
#include "FolderCompareView.h"
#include "SessionIO.h"
#include "ExportTools.h"
#include "AutoMerge.h"
#include "DocumentConverters.h"
#include "EngineOptions.h"
#include "FileFilters.h"
#include "Dialogs.h"
#include "SafeFileOps.h"
#include <QTabWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QTextBrowser>
#include <QProgressDialog>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QSettings>
#include <QMessageBox>
#include <QFile>
#include <QVBoxLayout>
#include <QTimer>

void MainWindow::openWebComparison(const QStringList &requested)
{
    QStringList urls=requested;
    if(urls.isEmpty()) {
        QDialog dialog(this);dialog.setWindowTitle(tr("Compare webpages"));
        auto *layout=new QFormLayout(&dialog);
        QList<QLineEdit*> fields;
        for(const QString &name:{tr("Left URL:"),tr("Right URL:"),tr("Third URL (optional):")}){
            auto *field=new QLineEdit(&dialog);field->setMinimumWidth(340);layout->addRow(name,field);fields.append(field);
        }
        auto *buttons=new QDialogButtonBox(QDialogButtonBox::Open|QDialogButtonBox::Cancel,&dialog);
        layout->addRow(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
        connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        if(dialog.exec()!=QDialog::Accepted)return;
        for(auto *field:fields)if(!field->text().trimmed().isEmpty())urls.append(field->text().trimmed());
    }
    if(urls.size()<2 || urls.size()>3){lm::warning(this,tr("Compare webpages"),tr("Enter two or three addresses."));return;}
    auto *view=new WebCompareView(urls,this);
    m_tabs->setCurrentIndex(m_tabs->addTab(view,view->tabTitle()));
    m_tabs->setTabToolTip(m_tabs->indexOf(view),urls.join(QLatin1Char('\n')));
    const auto keepSnapshots=[this](const QStringList &paths)->QStringList{
        auto temp=std::make_shared<QTemporaryDir>();QStringList copies;
        if(!temp->isValid())return copies;
        for(const QString &path:paths){
            const QString copy=temp->filePath(QFileInfo(path).fileName());
            if(!QFile::copy(path,copy))return {};
            copies.append(copy);
        }
        m_generatedDocuments.append(temp);return copies;
    };
    connect(view,&WebCompareView::openTextsRequested,this,[this,keepSnapshots](const QStringList &paths){
        const auto copies=keepSnapshots(paths);const int before=m_tabs->count();
        if(copies.size()==paths.size())openFileComparison(copies,QList<bool>(copies.size(),true),true);
        if(m_tabs->count()>before)m_tabs->currentWidget()->setProperty("MergeDeskTransientComparison",true);
    });
    connect(view,&WebCompareView::openImagesRequested,this,[this,keepSnapshots](const QStringList &paths){
        const auto copies=keepSnapshots(paths);const int before=m_tabs->count();
        if(copies.size()==paths.size())openImageComparison(copies,QList<bool>(copies.size(),true));
        if(m_tabs->count()>before)m_tabs->currentWidget()->setProperty("MergeDeskTransientComparison",true);
    });
    rememberComparison(urls);
}

void MainWindow::openDocumentComparison(const QStringList &paths)
{
    struct Result {lm::ConvertedDocuments converted;QString error;bool ok=false;};
    auto *watcher=new QFutureWatcher<Result>(this);
    auto *progress=new QProgressDialog(tr("Extracting document text…"),QString(),0,0,this);
    progress->setWindowTitle(tr("Compare documents"));progress->setWindowModality(Qt::WindowModal);progress->setMinimumDuration(0);progress->show();
    const QSettings settings;
    lm::ConversionOptions options;
    options.pdfToTextExecutable=settings.value(QStringLiteral("Converters/PdfToText")).toString();
    options.officeExecutable=settings.value(QStringLiteral("Converters/LibreOffice")).toString();
    const auto command=m_openingCommandLine?std::optional<lm::CommandLine>(*m_openingCommandLine):std::nullopt;
    const auto extraFields=m_openingProjectExtras;
    connect(watcher,&QFutureWatcher<Result>::finished,this,[this,watcher,progress,paths,command,extraFields]{
        const Result result=watcher->result();progress->close();progress->deleteLater();watcher->deleteLater();
        if(!result.ok){lm::warning(this,tr("Compare documents"),result.error);return;}
        m_generatedDocuments.append(result.converted.temporaryDirectory);
        const int before=m_tabs->count();
        openFileComparison(result.converted.paths,QList<bool>(result.converted.paths.size(),true),true);
        if(m_tabs->count()==before)return;
        QWidget *page=m_tabs->currentWidget();page->setProperty("MergeDeskSourcePaths",paths);
        page->setProperty("MergeDeskConvertedDocument",true);
        if(command){m_projectCommands[page]=*command;connect(page,&QObject::destroyed,this,[this,page]{m_projectCommands.remove(page);});}
        if(!extraFields.isEmpty()){m_projectExtras[page]=extraFields;connect(page,&QObject::destroyed,this,[this,page]{m_projectExtras.remove(page);});}
        describeSides(m_tabs->currentWidget(),result.converted.labels);
        if(!result.converted.warnings.isEmpty())lm::warning(this,tr("Document conversion notes"),result.converted.warnings.join(QStringLiteral("\n\n")));
    });
    watcher->setFuture(QtConcurrent::run([paths,options]{Result result;result.ok=lm::convertDocuments(paths,&result.converted,&result.error,options);return result;}));
}

void MainWindow::openProjectFile(const QString &requested)
{
    const QString path=requested.isEmpty()?QFileDialog::getOpenFileName(this,tr("Open comparison project"),{},tr("Comparison projects (*.WinMerge *.winmerge *.mergedesk);;All files (*)")):requested;
    if(path.isEmpty())return;
    lm::ProjectSession session;QString error;
    if(!lm::loadProject(path,&session,&error)){lm::warning(this,tr("Open comparison project"),error);return;}
    for(int i=0;i<session.comparisons.size();++i){
        const auto &command=session.comparisons.at(i);
        session.warnings.append(lm::useOptions(command.options));
        if(!command.fileFilter.isEmpty())lm::useFileFilterMask(command.fileFilter);
        if(command.compareMethod)lm::useCompareMethod(*command.compareMethod);
        lm::updateCodepageModule();
        m_openingProjectExtras=session.extraFields.value(i);
        const int before=m_tabs->count();openFromCommandLine(command);
        m_openingProjectExtras.clear();
        if(m_tabs->count()>before && i<session.extraFields.size()){
            QWidget *view=m_tabs->currentWidget();m_projectExtras[view]=session.extraFields[i];
            connect(view,&QObject::destroyed,this,[this,view]{m_projectExtras.remove(view);});
        }
    }
    if(!session.warnings.isEmpty())lm::warning(this,tr("Project import notes"),session.warnings.join(QStringLiteral("\n\n")));
}

void MainWindow::saveProjectFile(const QString &requested)
{
    lm::ProjectSession project;
    int omitted=0;
    for(int i=0;i<m_tabs->count();++i){
        QWidget *page=m_tabs->widget(i);
        if(page->property("MergeDeskTransientComparison").toBool()){++omitted;continue;}
        lm::CommandLine entry=m_projectCommands.value(page);
        entry.leftFlags&=lm::CommandLine::ReadOnly;entry.middleFlags&=lm::CommandLine::ReadOnly;entry.rightFlags&=lm::CommandLine::ReadOnly;
        if(auto *file=qobject_cast<FileCompareView*>(page)){
            entry.paths=file->paths();entry.windowType=lm::CommandLine::WindowType::Text;
            entry.leftDesc=file->sideCaption(0);entry.rightDesc=file->sideCaption(file->paneCount()-1);
            if(file->paneCount()==3)entry.middleDesc=file->sideCaption(1);
            if(file->isSideReadOnly(0))entry.leftFlags|=lm::CommandLine::ReadOnly;
            if(file->paneCount()==3 && file->isSideReadOnly(1))entry.middleFlags|=lm::CommandLine::ReadOnly;
            if(file->isSideReadOnly(file->paneCount()-1))entry.rightFlags|=lm::CommandLine::ReadOnly;
        }else if(auto *folder=qobject_cast<FolderCompareView*>(page)){
            entry.paths=folder->roots();entry.windowType=lm::CommandLine::WindowType::Folder;
            entry.recursive=folder->isRecursive();entry.fileFilter=folder->fileFilter();entry.compareMethod=lm::currentCompareMethod();
        }else if(auto *table=qobject_cast<TableCompareView*>(page)){entry.paths=table->paths();entry.windowType=lm::CommandLine::WindowType::Table;
        }else if(auto *image=qobject_cast<ImageCompareView*>(page)){entry.paths=image->paths();entry.windowType=lm::CommandLine::WindowType::Image;
        }else if(auto *hex=qobject_cast<HexCompareView*>(page)){entry.paths=hex->paths();entry.windowType=lm::CommandLine::WindowType::Binary;
        }else if(auto *web=qobject_cast<WebCompareView*>(page)){entry.paths=web->paths();entry.windowType=lm::CommandLine::WindowType::WebPage;}
        const auto readonly=[page](int side){
            if(auto *view=qobject_cast<FileCompareView*>(page))return view->isSideReadOnly(side);
            if(auto *view=qobject_cast<TableCompareView*>(page))return view->isSideReadOnly(side);
            if(auto *view=qobject_cast<ImageCompareView*>(page))return view->isSideReadOnly(side);
            if(auto *view=qobject_cast<HexCompareView*>(page))return view->isSideReadOnly(side);
            return false;
        };
        entry.leftFlags=readonly(0)?lm::CommandLine::ReadOnly:0;
        entry.middleFlags=entry.paths.size()==3&&readonly(1)?lm::CommandLine::ReadOnly:0;
        entry.rightFlags=readonly(entry.paths.size()-1)?lm::CommandLine::ReadOnly:0;
        const QStringList sourcePaths=page->property("MergeDeskSourcePaths").toStringList();
        if(!sourcePaths.isEmpty())entry.paths=sourcePaths;
        if(page->property("MergeDeskConvertedDocument").toBool())entry.windowType=lm::CommandLine::WindowType::Automatic;
        if(entry.paths.size()<2 || entry.paths.contains(QString())){
            if(auto *file=qobject_cast<FileCompareView*>(page);file&&file->isModified())++omitted;
            continue;
        }
        const auto options=lm::currentDiffOptions();
        entry.options={
            {QStringLiteral("Settings/IgnoreSpace"),QString::number(options.nIgnoreWhitespace)},
            {QStringLiteral("Settings/IgnoreBlankLines"),QString::number(options.bIgnoreBlankLines)},
            {QStringLiteral("Settings/IgnoreCase"),QString::number(options.bIgnoreCase)},
            {QStringLiteral("Settings/IgnoreEol"),QString::number(options.bIgnoreEol)},
            {QStringLiteral("Settings/IgnoreNumbers"),QString::number(options.bIgnoreNumbers)},
            {QStringLiteral("Settings/IgnoreCodepage"),QString::number(lm::ignoreCodepageDifferences())},
            {QStringLiteral("Settings/FilterCommentsLines"),QString::number(options.bFilterCommentsLines)},
            {QStringLiteral("Settings/IgnoreMissingTrailingEol"),QString::number(options.bIgnoreMissingTrailingEol)},
            {QStringLiteral("Settings/IgnoreLineBreaks"),QString::number(options.bIgnoreLineBreaks)}
        };
        project.comparisons.append(entry);project.extraFields.append(m_projectExtras.value(page));
    }
    if(project.comparisons.isEmpty()){lm::warning(this,tr("Save comparison project"),tr("Open a comparison with named files, folders, or webpages first."));return;}
    const QString path=requested.isEmpty()?QFileDialog::getSaveFileName(this,tr("Save comparison project"),QStringLiteral("Comparison.WinMerge"),tr("Comparison projects (*.WinMerge);;All files (*)")):requested;
    if(path.isEmpty())return;
    QString error;
    if(!lm::saveProject(path,project,&error))lm::warning(this,tr("Save comparison project"),error);
    else if(omitted>0)lm::warning(this,tr("Save comparison project"),tr("Temporary snapshots and unnamed buffers cannot be reopened from a project. Save or export them separately."));
}

void MainWindow::exportComparison(bool patch,bool csv)
{
    QWidget *page=m_tabs->currentWidget();QString error;
    if(auto *folder=qobject_cast<FolderCompareView*>(page)){
        if(patch){lm::warning(this,tr("Create patch"),tr("Open a text comparison to create a unified patch."));return;}
        if(folder->isComparing()){lm::warning(this,tr("Export report"),tr("Wait for the folder comparison to finish."));return;}
        const QString file=QFileDialog::getSaveFileName(this,tr("Export folder report"),csv?QStringLiteral("comparison.csv"):QStringLiteral("comparison.html"),csv?tr("CSV files (*.csv)"):tr("HTML files (*.html)"));
        if(file.isEmpty())return;
        const bool ok=csv?lm::exportFolderCsvReport(folder->result(),folder->roots(),file,&error):lm::exportFolderHtmlReport(folder->result(),folder->roots(),file,&error);
        if(!ok)lm::warning(this,tr("Export report"),error);return;
    }
    lm::TextExportInput input;
    QList<QByteArray> snapshots;
    if(auto *file=qobject_cast<FileCompareView*>(page)){
        input.paths=file->paths();for(int i=0;i<file->paneCount();++i)snapshots.append(file->utf8Snapshot(i));
    }else if(auto *table=qobject_cast<TableCompareView*>(page)){
        input.paths=table->paths();for(int i=0;i<input.paths.size();++i)snapshots.append(table->utf8Snapshot(i));
    }else {lm::warning(this,tr("Export report"),tr("Open a text, table, or folder comparison first."));return;}
    if(csv){lm::warning(this,tr("Export report"),tr("CSV reports are available for folder comparisons."));return;}
    if(input.paths.size()==3){
        const QStringList pairs{tr("Left → Middle"),tr("Middle → Right"),tr("Left → Right")};bool ok=false;
        const QString chosen=QInputDialog::getItem(this,tr("Export compared pair"),tr("Choose the source and destination:"),pairs,2,false,&ok);if(!ok)return;
        const int choice=pairs.indexOf(chosen),a=choice==1?1:0,b=choice==0?1:2;
        input.paths={input.paths[a],input.paths[b]};input.utf8Snapshots={snapshots[a],snapshots[b]};
    }else input.utf8Snapshots=snapshots;
    for(int i=0;i<input.paths.size();++i)input.labels.append(input.paths[i].isEmpty()?tr("Untitled %1").arg(i+1):QFileInfo(input.paths[i]).fileName());
    const QString file=QFileDialog::getSaveFileName(this,patch?tr("Create patch"):tr("Export HTML report"),patch?QStringLiteral("changes.patch"):QStringLiteral("comparison.html"),patch?tr("Unified patches (*.patch *.diff)"):tr("HTML files (*.html)"));
    if(file.isEmpty())return;
    const bool ok=patch?lm::exportUnifiedPatch(input,file,&error):lm::exportHtmlReport(input,file,&error);
    if(!ok)lm::warning(this,patch?tr("Create patch"):tr("Export report"),error);
}

void MainWindow::autoMergeInto(int target)
{
    auto *file=qobject_cast<FileCompareView*>(m_tabs->currentWidget());
    if(!file || file->paneCount()!=3){lm::warning(this,tr("Auto merge"),tr("Open three text files. The destination is treated as their common ancestor."));return;}
    if(target<0){
        const QStringList sides{tr("Left"),tr("Middle"),tr("Right")};bool ok=false;
        const QString selected=QInputDialog::getItem(this,tr("Auto merge"),tr("Common ancestor and destination pane:"),sides,1,false,&ok);if(!ok)return;target=sides.indexOf(selected);
    }
    if(file->isSideReadOnly(target)){lm::warning(this,tr("Auto merge"),tr("The destination pane is read-only."));return;}
    QList<int> other;for(int i=0;i<3;++i)if(i!=target)other.append(i);
    const auto result=md::mergeText(file->utf8Snapshot(target),file->utf8Snapshot(other[0]),file->utf8Snapshot(other[1]));
    if(!result.error.isEmpty()){lm::warning(this,tr("Auto merge"),result.error);return;}
    file->replaceContents(target,QString::fromUtf8(result.text));
    if(result.conflicts>0)lm::warning(this,tr("Merge conflicts"),tr("%1 conflict(s) remain between LEFT, BASE, and RIGHT markers. Review and resolve them before saving.").arg(result.conflicts));
}

void MainWindow::showManual()
{
    QDialog dialog(this);dialog.setWindowTitle(tr("MergeDesk Manual"));dialog.resize(860,650);
    auto *layout=new QVBoxLayout(&dialog);auto *browser=new QTextBrowser(&dialog);browser->setOpenExternalLinks(true);
    QFile manual(QStringLiteral(":/about/MANUAL.md"));if(manual.open(QIODevice::ReadOnly))browser->setMarkdown(QString::fromUtf8(manual.readAll()));
    layout->addWidget(browser);auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);dialog.exec();
}
