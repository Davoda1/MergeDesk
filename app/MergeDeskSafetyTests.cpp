// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MergeDeskSafetyTests.h"
#include "MergeDeskBufferTests.h"
#include "MergeDeskEncodingTests.h"
#include "SafeFileOps.h"
#include "ArchiveCompare.h"
#include "FileOps.h"
#include "AutoMerge.h"
#include "FileCompareView.h"
#include "WebCompareView.h"
#include "MainWindow.h"
#include "SessionIO.h"
#include "MessageBoxes.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebEnginePage>
#include <QWebEngineHistory>
#include <QWebEngineView>
#include <functional>
#include <memory>
#include <archive.h>
#include <archive_entry.h>

namespace md {
namespace {
struct Checks {
    int passed=0,failed=0;
    void expect(bool value,const char *name,const QString &detail={}) {
        if(value){++passed;return;}
        ++failed;
        QTextStream(stderr)<<"FAIL: "<<name<<(detail.isEmpty()?QString():QStringLiteral(": ")+detail)<<'\n';
    }
    int finish(const char *name) {
        QTextStream(stdout)<<name<<": "<<passed<<" passed, "<<failed<<" failed\n";
        return failed?1:0;
    }
};
bool put(const QString &path,const QByteArray &bytes) {
    QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size();
}
QByteArray get(const QString &path) {
    QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();
}
bool waitUntil(const std::function<bool()> &condition,int timeout=20000) {
    QElapsedTimer clock;clock.start();
    while(!condition()&&clock.elapsed()<timeout){
        QCoreApplication::processEvents(QEventLoop::AllEvents,20);
        QThread::msleep(5);
    }
    return condition();
}

struct ArchiveFixtureEntry {
    QString name;
    unsigned int type=AE_IFREG;
    QByteArray contents;
    QString hardlink;
    QString symlink;
};
la_ssize_t writeArchiveFixture(struct archive *,void *file,const void *bytes,size_t size) {
    return static_cast<QFile *>(file)->write(static_cast<const char *>(bytes),static_cast<qint64>(size));
}
bool makeArchiveFixture(const QString &path,const QList<ArchiveFixtureEntry> &entries) {
    QFile file(path);
    if(!file.open(QIODevice::WriteOnly))return false;
    std::unique_ptr<archive,decltype(&archive_write_free)> writer(archive_write_new(),archive_write_free);
    if(!writer||archive_write_set_format_pax_restricted(writer.get())!=ARCHIVE_OK
        ||archive_write_open(writer.get(),&file,nullptr,writeArchiveFixture,nullptr)!=ARCHIVE_OK)return false;
    for(const auto &item:entries){
        std::unique_ptr<archive_entry,decltype(&archive_entry_free)> entry(archive_entry_new(),archive_entry_free);
        if(!entry)return false;
        archive_entry_set_pathname(entry.get(),QFile::encodeName(item.name).constData());
        archive_entry_set_filetype(entry.get(),item.type);
        archive_entry_set_perm(entry.get(),item.type==AE_IFDIR?0700:0600);
        archive_entry_set_size(entry.get(),item.contents.size());
        if(!item.hardlink.isEmpty())archive_entry_set_hardlink(entry.get(),QFile::encodeName(item.hardlink).constData());
        if(!item.symlink.isEmpty())archive_entry_set_symlink(entry.get(),QFile::encodeName(item.symlink).constData());
        if(archive_write_header(writer.get(),entry.get())!=ARCHIVE_OK)return false;
        if(!item.contents.isEmpty()&&archive_write_data(writer.get(),item.contents.constData(),item.contents.size())!=item.contents.size())return false;
    }
    return archive_write_close(writer.get())==ARCHIVE_OK;
}

void archiveSafetyChecks(Checks &check,const QString &directory) {
    const QString outside=QDir(directory).filePath(QStringLiteral("outside.txt"));
    check.expect(put(outside,"outside sentinel\n"),"create outside archive sentinel");
    const QString archive=QDir(directory).filePath(QStringLiteral("fixture.tar"));
    QString error;
    check.expect(makeArchiveFixture(archive,{
        {QStringLiteral("."),AE_IFDIR,{}, {}, {}},
        {QStringLiteral("sub"),AE_IFDIR,{}, {}, {}},
        {QStringLiteral("sub/payload.txt"),AE_IFREG,"ordinary archive content\n",{}, {}}}),
        "create ordinary archive fixture");
    QTemporaryDir ordinary(QDir(directory).filePath(QStringLiteral("ordinary-XXXXXX")));
    check.expect(ordinary.isValid()&&lm::extractArchive(archive,ordinary.path(),&error)
        &&get(ordinary.filePath(QStringLiteral("sub/payload.txt")))=="ordinary archive content\n",
        "regular archive files and directories extract",error);
    check.expect(!lm::extractArchive(archive,ordinary.path(),&error)
        &&get(ordinary.filePath(QStringLiteral("sub/payload.txt")))=="ordinary archive content\n",
        "nonempty extraction destination is refused");

    const QList<ArchiveFixtureEntry> unsafeEntries {
        {QStringLiteral("entry.txt"),AE_IFREG,{},outside,{}},
        {QStringLiteral("entry.txt"),AE_IFREG,{},QStringLiteral("../outside.txt"),{}},
        {QStringLiteral("entry.txt"),AE_IFLNK,{}, {},outside},
        {QStringLiteral("entry.txt"),AE_IFLNK,{}, {},QStringLiteral("../outside.txt")},
        {QStringLiteral("entry.txt"),AE_IFIFO,{}, {},{}},
        {QStringLiteral("../outside.txt"),AE_IFREG,"replacement\n",{},{}},
        {outside,AE_IFREG,"replacement\n",{}, {}}
    };
    for(qsizetype i=0;i<unsafeEntries.size();++i){
        check.expect(makeArchiveFixture(archive,{unsafeEntries.at(i)}),"create unsafe archive fixture",QString::number(i));
        QTemporaryDir extracted(QDir(directory).filePath(QStringLiteral("unsafe-XXXXXX")));
        check.expect(extracted.isValid()&&!lm::extractArchive(archive,extracted.path(),&error)&&!error.isEmpty()
            &&QDir(extracted.path()).entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot).isEmpty()
            &&get(outside)=="outside sentinel\n",
            "unsafe archive entry creates no link or special file and preserves outside contents",QString::number(i)+QStringLiteral(": ")+error);
    }
    check.expect(makeArchiveFixture(archive,{{QStringLiteral("ordinary.txt"),AE_IFREG,"cancelled\n",{}, {}}}),
        "create cancellable archive fixture");
    QTemporaryDir cancelled(QDir(directory).filePath(QStringLiteral("cancelled-XXXXXX")));
    check.expect(cancelled.isValid()&&!lm::extractArchive(archive,cancelled.path(),&error,[](const QString &){return false;})
        &&error.isEmpty()&&!QFileInfo::exists(cancelled.filePath(QStringLiteral("ordinary.txt"))),
        "archive cancellation leaves error empty and writes no entry");

    const QString innerArchive=QDir(directory).filePath(QStringLiteral("inner.tar"));
    check.expect(makeArchiveFixture(innerArchive,{
        {QStringLiteral(".lm-inner-inner.tar"),AE_IFREG,"legitimate filename\n",{}, {}},
        {QStringLiteral("keep.txt"),AE_IFREG,"second entry\n",{}, {}}})
        &&makeArchiveFixture(archive,{{QStringLiteral("inner.tar"),AE_IFREG,get(innerArchive),{}, {}}}),
        "create nested archive with staging-name collision");
    QTemporaryDir nested(QDir(directory).filePath(QStringLiteral("nested-XXXXXX")));
    check.expect(nested.isValid()&&lm::extractArchive(archive,nested.path(),&error)
        &&get(nested.filePath(QStringLiteral(".lm-inner-inner.tar")))=="legitimate filename\n"
        &&get(nested.filePath(QStringLiteral("keep.txt")))=="second entry\n",
        "nested archive staging does not overwrite or remove extracted entries",error);
    check.expect(makeArchiveFixture(archive,{
        {QStringLiteral("inner.tar"),AE_IFREG,get(innerArchive),{}, {}},
        {QStringLiteral(".metadata"),AE_IFREG,"metadata\n",{}, {}}}),
        "create archive with a nested archive and hidden metadata");
    QTemporaryDir multiple(QDir(directory).filePath(QStringLiteral("multiple-XXXXXX")));
    check.expect(multiple.isValid()&&lm::extractArchive(archive,multiple.path(),&error)
        &&get(multiple.filePath(QStringLiteral("inner.tar")))==get(innerArchive)
        &&get(multiple.filePath(QStringLiteral(".metadata")))=="metadata\n"
        &&!QFileInfo::exists(multiple.filePath(QStringLiteral("keep.txt"))),
        "hidden entries prevent single-file nested archive unwrapping",error);

    // A deterministic stored ZIP with one payload.txt entry and a known CRC.
    const QByteArray zipPayload("Integrity sentinel: expected content.\n");
    const QByteArray storedZip=QByteArray::fromHex(
        "504b0304140000000000000021288777d61f26000000260000000b0000007061796c6f61642e747874496e74"
        "6567726974792073656e74696e656c3a20657870656374656420636f6e74656e742e0a504b01021403140000"
        "000000000021288777d61f26000000260000000b00000000000000000000008001000000007061796c6f6164"
        "2e747874504b05060000000001000100390000004f0000000000");
    const QString zipArchive=QDir(directory).filePath(QStringLiteral("integrity.zip"));
    check.expect(put(zipArchive,storedZip),"create valid stored ZIP fixture");
    QTemporaryDir validZip(QDir(directory).filePath(QStringLiteral("valid-zip-XXXXXX")));
    check.expect(validZip.isValid()&&lm::extractArchive(zipArchive,validZip.path(),&error)
        &&get(validZip.filePath(QStringLiteral("payload.txt")))==zipPayload,
        "valid stored ZIP extracts byte-exact content",error);

    QByteArray wrongCrc=storedZip;
    wrongCrc[14]^=1; // CRC in the local file header.
    wrongCrc[79+16]^=1; // Matching CRC in the central directory header.
    check.expect(put(zipArchive,wrongCrc),"create stored ZIP with incorrect CRC");
    QTemporaryDir crcMismatch(QDir(directory).filePath(QStringLiteral("wrong-crc-XXXXXX")));
    check.expect(crcMismatch.isValid()&&!lm::extractArchive(zipArchive,crcMismatch.path(),&error)
        &&!error.isEmpty(),"stored ZIP with incorrect CRC is rejected",error);

    QByteArray corruptPayload=storedZip;
    corruptPayload[30+11]^=1; // First payload byte, after the header and filename; CRC unchanged.
    check.expect(put(zipArchive,corruptPayload),"create stored ZIP with damaged payload");
    QTemporaryDir payloadMismatch(QDir(directory).filePath(QStringLiteral("bad-payload-XXXXXX")));
    check.expect(payloadMismatch.isValid()&&!lm::extractArchive(zipArchive,payloadMismatch.path(),&error)
        &&!error.isEmpty(),"stored ZIP with damaged payload is rejected",error);
}
}

int runSafetyTests()
{
    // Project import warnings are expected test evidence, never a modal UI.
    const bool wasUnattended=lm::unattended();
    lm::setUnattended(true);
    Checks check;QTemporaryDir dir;
    if(!dir.isValid())return 2;
    const QString path=dir.filePath(QString::fromUtf8("árvíztűrő — über espace.txt"));
    const QString destination=dir.filePath(QStringLiteral("copied.txt"));
    QString error;
    check.expect(put(path,"original\r\n"),"create Unicode fixture");
    check.expect(createBackup(path,&error)&&get(path+QStringLiteral(".bak"))=="original\r\n","backup retains original bytes",error);
    check.expect(writeBytesAtomic(path,"changed\n",&error)&&get(path)=="changed\n","atomic write supports Unicode path",error);
    check.expect(createBackup(path,&error)&&get(path+QStringLiteral(".bak"))=="changed\n","second backup updates latest backup",error);
    const auto previous=QDir(dir.path()).entryList({QFileInfo(path).fileName()+QStringLiteral(".bak.*")},QDir::Files);
    check.expect(previous.size()==1&&get(dir.filePath(previous.value(0)))=="original\r\n","older backup is retained separately");
    check.expect(copyFileAtomic(path,destination,true,&error)&&get(destination)=="changed\n","copy commits complete bytes",error);
    check.expect(!copyFileAtomic(path,path,false,&error)&&get(path)=="changed\n","same-file copy is refused");
    check.expect(!writeBytesAtomic(dir.path(),"x",&error),"directory destination is refused");

    const QString sourceDirectory=dir.filePath(QStringLiteral("copy-source"));
    const QString copiedDirectory=dir.filePath(QStringLiteral("copy-target"));
    check.expect(QDir().mkpath(sourceDirectory)&&put(QDir(sourceDirectory).filePath(QStringLiteral("keep.txt")),"copy sentinel\n"),
        "create recursive copy fixture");
    check.expect(lm::copyRecursively(sourceDirectory,copiedDirectory)
        &&get(QDir(copiedDirectory).filePath(QStringLiteral("keep.txt")))=="copy sentinel\n",
        "recursive copy retains ordinary files");
    check.expect(!lm::copyRecursively(sourceDirectory,sourceDirectory)
        &&!lm::copyRecursively(sourceDirectory,QDir(sourceDirectory).filePath(QStringLiteral("child")))
        &&!QFileInfo::exists(QDir(sourceDirectory).filePath(QStringLiteral("child"))),
        "recursive copy refuses itself and its descendants before creating directories");
#ifdef Q_OS_WIN
    const QString caseVariant=QDir(dir.path()).filePath(QStringLiteral("COPY-SOURCE/child"));
    check.expect(!lm::copyRecursively(sourceDirectory,caseVariant)
        &&!QFileInfo::exists(QDir(sourceDirectory).filePath(QStringLiteral("child"))),
        "Windows case variants cannot copy a folder inside itself");
#endif
    archiveSafetyChecks(check,dir.path());

    check.expect(writeTextAtomic(path,{QString::fromUtf8("café"),QStringLiteral("last")},{QStringLiteral("\r\n"),QStringLiteral("\n")},false,0,1252,false,&error)
        &&get(path)==QByteArray("caf\xe9\r\nlast"),"legacy encoding and mixed final-newline state",error);
    const QByteArray preserved=get(path);
    check.expect(!writeTextAtomic(path,{QString::fromUtf8("cannot encode 😀")},{QStringLiteral("\n")},true,0,1252,false,&error)
        &&get(path)==preserved,"unrepresentable character leaves original intact");
    check.expect(writeTextAtomic(path,{QString::fromUtf8("ő"),QStringLiteral("B")},{QStringLiteral("\r\n"),QStringLiteral("\n")},true,1,1200,true,&error)
        &&get(path)==QByteArray::fromHex("fffE51010d000a0042000a00"),"UTF-16 LE BOM and line endings",error);
    check.expect(writeTextAtomic(path,{QStringLiteral("one"),QStringLiteral("two"),QStringLiteral("three")},
        {QStringLiteral("\r\n"),QStringLiteral("\r"),QStringLiteral("\n")},true,3,65001,true,&error)
        &&get(path)==QByteArray::fromHex("efbbbf")+"one\r\ntwo\rthree\n","mixed EOL and UTF-8 BOM survive write",error);
    check.expect(writeTextAtomic(path,{}, {},false,3,65001,false,&error)&&get(path).isEmpty(),"empty buffer writes empty file",error);
#ifdef Q_OS_UNIX
    const QString link=dir.filePath(QStringLiteral("symbolic-link"));
    check.expect(QFile::link(destination,link),"create link fixture");
    const QByteArray targetBefore=get(destination);
    check.expect(!writeBytesAtomic(link,"bad",&error)&&get(destination)==targetBefore,"symlink destination cannot overwrite target");
    const QString realDirectory=dir.filePath(QStringLiteral("real-directory"));
    QDir().mkpath(realDirectory);
    const QString linkedDirectory=dir.filePath(QStringLiteral("linked-directory"));
    check.expect(QFile::link(realDirectory,linkedDirectory),"create directory link fixture");
    check.expect(!writeBytesAtomic(linkedDirectory+QStringLiteral("/new.txt"),"bad",&error)
        &&!QFileInfo::exists(realDirectory+QStringLiteral("/new.txt")),"linked parent directory is refused");
#endif

    const QByteArray base="one\nstable A\nstable B\nstable C\nlast\n";
    const QByteArray left="ONE\nstable A\nstable B\nstable C\nlast\n";
    const QByteArray right="one\nstable A\nstable B\nstable C\nLAST\n";
    const QByteArray expected="ONE\nstable A\nstable B\nstable C\nLAST\n";
    const auto merged=mergeText(base,left,right);
    check.expect(merged.error.isEmpty()&&merged.conflicts==0&&merged.text==expected,"independent changes merge without conflict",merged.error);
    const auto same=mergeText(base,left,left);
    check.expect(same.error.isEmpty()&&same.conflicts==0&&same.text==left,"identical changes merge once",same.error);
    const auto conflict=mergeText("base\n","left\n","right\n");
    check.expect(conflict.error.isEmpty()&&conflict.conflicts==1&&conflict.text.contains("<<<<<<< LEFT")
        &&conflict.text.contains("||||||| BASE")&&conflict.text.contains(">>>>>>> RIGHT"),"conflicting changes retain left, base and right",conflict.error);
    const auto noFinal=mergeText("first\nlast","FIRST\nlast","first\nlast");
    check.expect(noFinal.conflicts==0&&noFinal.text=="FIRST\nlast","merge preserves missing final newline");
    const auto empty=mergeText({}, {}, {});
    check.expect(empty.error.isEmpty()&&empty.conflicts==0&&empty.text.isEmpty(),"empty merge is valid");

    const QString basePath=dir.filePath(QStringLiteral("base.txt"));
    const QString leftPath=dir.filePath(QStringLiteral("left.txt"));
    const QString rightPath=dir.filePath(QStringLiteral("right.txt"));
    put(basePath,base);put(leftPath,left);put(rightPath,right);
    FileCompareView view;
    check.expect(view.compare({leftPath,basePath,rightPath},&error),"open three-pane merge fixture",error);
    check.expect(view.utf8Snapshot(1)==base,"current buffer snapshot has exact final newline");
    view.replaceContents(1,QString::fromUtf8(expected));
    check.expect(view.utf8Snapshot(1)==expected&&view.isModified(),"automatic merge replaces buffer once");
    check.expect(get(basePath)==base,"automatic merge does not save before review");
    view.undoActive();
    check.expect(view.utf8Snapshot(1)==base,"whole automatic merge can be undone");
    view.redoActive();
    check.expect(view.utf8Snapshot(1)==expected,"whole automatic merge can be redone");
    check.expect(view.saveSideAt(1,&error)&&get(basePath)==expected,"reviewed merge saves exact bytes",error);

    const QString projectLeft=dir.filePath(QStringLiteral("project-left.txt"));
    const QString projectRight=dir.filePath(QStringLiteral("project-right.txt"));
    put(projectLeft,"Case-sensitive text\n");put(projectRight,"CASE-SENSITIVE TEXT\n");
    lm::ProjectSession project;lm::CommandLine command;
    command.paths={projectLeft,projectRight};command.windowType=lm::CommandLine::WindowType::Text;
    command.leftFlags=lm::CommandLine::ReadOnly;command.leftDesc=QStringLiteral("Original project input");
    command.options={{QStringLiteral("Settings/IgnoreCase"),QStringLiteral("1")}};
    project.comparisons.append(command);
    project.extraFields.append({{QStringLiteral("hidden-list"),QStringLiteral("<hidden-list><hidden-item>retained.txt</hidden-item></hidden-list>")}});
    const QString projectPath=dir.filePath(QStringLiteral("session.WinMerge"));
    check.expect(lm::saveProject(projectPath,project,&error),"prepare imported project fixture",error);
    MainWindow window;window.openProjectFile(projectPath);
    const auto projectView=window.findChild<FileCompareView*>();
    check.expect(projectView&&projectView->diffCount()==0&&projectView->isSideReadOnly(0)
        &&projectView->sideCaption(0)==command.leftDesc,"project opening applies comparison options, descriptions and read-only flags");
    const QString savedProject=dir.filePath(QStringLiteral("resaved.WinMerge"));
    window.saveProjectFile(savedProject);
    lm::ProjectSession roundtrip;
    check.expect(lm::loadProject(savedProject,&roundtrip,&error)&&roundtrip.comparisons.size()==1,
        "save current GUI comparison as project",error);
    if(roundtrip.comparisons.size()==1){
        const auto &saved=roundtrip.comparisons.first();
        check.expect(saved.paths==command.paths&&saved.leftFlags==command.leftFlags&&saved.leftDesc==command.leftDesc,
            "GUI project save preserves file identity and restrictions");
        check.expect(roundtrip.extraFields.first().value(QStringLiteral("hidden-list")).contains(QStringLiteral("retained.txt")),
            "GUI project save preserves unsupported upstream fields");
    }
    const int safetyStatus=check.finish("MergeDesk safety");
    const int bufferStatus=runBufferTests();
    const int tableStatus=runTableReadOnlyTests();
    const int encodingStatus=runEncodingTests();
    lm::setUnattended(wasUnattended);
    return safetyStatus||bufferStatus||tableStatus||encodingStatus?1:0;
}

int runWebTests()
{
    Checks check;QTemporaryDir dir;
    if(!dir.isValid())return 2;
    const QString pages=dir.filePath(QStringLiteral("pages"));
    const QString first=QDir(pages).filePath(QStringLiteral("left.html"));
    const QString second=QDir(pages).filePath(QStringLiteral("right.html"));
    check.expect(QDir().mkpath(pages)&&put(dir.filePath(QStringLiteral("private.txt")),"not-page-data"),
        "create private file outside the selected HTML folder");
    check.expect(put(QDir(pages).filePath(QStringLiteral("local-style.css")),"body{background:#edf4ff;color:#192b4d;font:20px sans-serif}")
        &&put(QDir(pages).filePath(QStringLiteral("data.json")),"{\"value\":\"local data\"}"),
        "create relative stylesheet and JSON fixtures");
    QTcpServer canary;
    const auto networkConnections=std::make_shared<int>(0);
    QObject::connect(&canary,&QTcpServer::newConnection,&canary,[&canary,networkConnections]{
        while(canary.hasPendingConnections()){
            QTcpSocket *socket=canary.nextPendingConnection();++*networkConnections;
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket]{
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 6\r\nConnection: close\r\n\r\ncanary");
                socket->disconnectFromHost();
            });
        }
    });
    check.expect(canary.listen(QHostAddress::LocalHost,0),"start local outbound-request canary",canary.errorString());
    if(!canary.isListening())return check.finish("MergeDesk web");
    const QByteArray origin="127.0.0.1:"+QByteArray::number(canary.serverPort());
    const auto jsString=[](const QString &text){
        const QByteArray array=QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
        return array.mid(1,array.size()-2);
    };
    const QByteArray outsideUrl=jsString(QUrl::fromLocalFile(dir.filePath(QStringLiteral("private.txt"))).toString());
    const QByteArray insideUrl=jsString(QUrl::fromLocalFile(QDir(pages).filePath(QStringLiteral("data.json"))).toString());
    const QByteArray securityScript=
        "<script>const mark=(k,v)=>document.documentElement.dataset[k]=v;"
        "const read=(u,k)=>fetch(u).then(r=>{if(!r.ok)throw Error('fetch');return r.text();}).then(t=>mark(k,t),()=>mark(k,'blocked'));"
        "read("+outsideUrl+",'outsideRead');read('data.json','insideRead');"
        "read('data:text/plain,inline-data','dataRead');"
        "read(URL.createObjectURL(new Blob(['blob-data'],{type:'text/plain'})),'blobDataRead');"
        "read('http://"+origin+"/fetch','remoteRead');"
        "const image=new Image;image.onload=image.onerror=()=>mark('remoteImage','done');image.src='http://"+origin+"/image';"
        "try{const socket=new WebSocket('ws://"+origin+"/socket');socket.onerror=()=>mark('remoteSocket','done');"
        "socket.onopen=()=>{mark('remoteSocket','done');socket.close();};}catch(e){mark('remoteSocket','done');}"
        "navigator.sendBeacon('http://"+origin+"/beacon','test');</script>";
    const QByteArray workerCode="onmessage=e=>Promise.all(e.data.map(p=>fetch(p).then(r=>{if(!r.ok)throw Error(\"fetch\");return r.text();}).catch(()=>\"blocked\"))).then(t=>postMessage(t));";
    check.expect(put(QDir(pages).filePath(QStringLiteral("worker.js")),workerCode),"create local worker access fixture");
    // Absolute file URLs test Chromium's native file boundary, independently
    // of how relative parent paths resolve within the selected folder.
    const QByteArray workerScript="<script>const targets=["+outsideUrl+",new URL('data.json',location.href).href,"+insideUrl+",'http://"+origin+"/worker'];"
        "const startWorker=(w,key)=>{w.onmessage=e=>document.documentElement.dataset[key]=JSON.stringify(e.data);w.onerror=()=>document.documentElement.dataset[key]='error';w.postMessage(targets);};"
        "try{startWorker(new Worker('worker.js'),'workerRead');}catch(e){document.documentElement.dataset.workerRead='error';}"
        "try{startWorker(new Worker(URL.createObjectURL(new Blob(['"+workerCode+"'],{type:'text/javascript'}))),'blobRead');}catch(e){document.documentElement.dataset.blobRead='error';}</script>";
    put(first,"<!doctype html><meta charset='utf-8'><link rel='stylesheet' href='local-style.css'><style>section{height:1700px}</style>"
        +securityScript+workerScript+"<h1>MergeDesk page A</h1><p id='value'>Original text</p><iframe id='outside-frame' hidden></iframe>"
        "<script>document.getElementById('outside-frame').src="+outsideUrl+";</script>"
        "<section>Bottom marker A</section>");
    put(second,"<!doctype html><meta charset='utf-8'><style>body{background:#fff1df;color:#542f13;font:20px sans-serif}section{height:1900px}</style><h1>MergeDesk page B</h1><p id='value'>Changed text</p><section>Bottom marker B</section>");
    WebCompareView view({first,second});
    view.resize(1050,700);
    bool loaded=false,success=false;
    QObject::connect(&view,&WebCompareView::allPagesLoaded,&view,[&](bool ok){loaded=true;success=ok;});
    view.show();
    check.expect(waitUntil([&]{return loaded;})&&success,"two local webpages render successfully");
    if(!loaded||!success)return check.finish("MergeDesk web");
    check.expect(view.paneForTest(0)&&view.paneForTest(1)&&!view.paneForTest(2),"two native web panes");
    check.expect(QUrl(view.paths().value(0)).toLocalFile()==QFileInfo(first).canonicalFilePath()
        &&QUrl(view.paths().value(1)).toLocalFile()==QFileInfo(second).canonicalFilePath(),
        "local webpage addresses retain ordinary filesystem URLs");
    struct LocalAccessResult { QVariantMap result; bool pending=false; };
    const auto localAccess=std::make_shared<LocalAccessResult>();
    const bool localAccessCompleted=waitUntil([&]{
        if(!localAccess->pending){
            localAccess->pending=true;
            view.paneForTest(0)->page()->runJavaScript(QStringLiteral("(()=>{const d=document.documentElement.dataset;return {outside:d.outsideRead||'',inside:d.insideRead||'',data:d.dataRead||'',blob:d.blobDataRead||'',remote:d.remoteRead||'',image:d.remoteImage||'',socket:d.remoteSocket||''};})()"),
                [localAccess](const QVariant &value){localAccess->result=value.toMap();localAccess->pending=false;});
        }
        const auto &values=localAccess->result;
        return !values.value(QStringLiteral("outside")).toString().isEmpty()&&!values.value(QStringLiteral("inside")).toString().isEmpty()
            &&!values.value(QStringLiteral("data")).toString().isEmpty()&&!values.value(QStringLiteral("blob")).toString().isEmpty()
            &&!values.value(QStringLiteral("remote")).toString().isEmpty()&&!values.value(QStringLiteral("image")).toString().isEmpty()
            &&!values.value(QStringLiteral("socket")).toString().isEmpty();
    });
    check.expect(localAccessCompleted&&localAccess->result.value(QStringLiteral("outside")).toString()==QStringLiteral("blocked"),
        "compared HTML cannot fetch a file outside its selected folder");
    check.expect(localAccessCompleted&&localAccess->result.value(QStringLiteral("inside")).toString()==QStringLiteral("{\"value\":\"local data\"}"),
        "compared HTML retains relative local JSON requests");
    check.expect(localAccessCompleted&&localAccess->result.value(QStringLiteral("data")).toString()==QStringLiteral("inline-data")
        &&localAccess->result.value(QStringLiteral("blob")).toString()==QStringLiteral("blob-data"),
        "local pages retain in-memory data and blob fetches");
    check.expect(localAccessCompleted&&localAccess->result.value(QStringLiteral("remote")).toString()==QStringLiteral("blocked")
        &&*networkConnections==0,"local HTML makes no outbound fetch, image, beacon or WebSocket connection",
        QStringLiteral("connections: %1").arg(*networkConnections));
    const auto frame=std::make_shared<QString>();
    view.paneForTest(0)->page()->runJavaScript(QStringLiteral("(()=>{try{return document.getElementById('outside-frame').contentDocument.body.textContent||'empty';}catch(e){return 'blocked';}})()"),
        [frame](const QVariant &value){*frame=value.toString();});
    check.expect(waitUntil([&]{return !frame->isEmpty();})&&!frame->contains(QStringLiteral("not-page-data")),
        "local iframe cannot expose a file outside the selected folder",*frame);
#ifdef Q_OS_UNIX
    check.expect(QFile::link(dir.filePath(QStringLiteral("private.txt")),QDir(pages).filePath(QStringLiteral("linked.txt"))),
        "create local resource symlink escape fixture");
    const auto linked=std::make_shared<QString>();
    view.paneForTest(0)->page()->runJavaScript(QStringLiteral("fetch('linked.txt').then(r=>{if(!r.ok)throw Error('fetch');return r.text();}).then(t=>document.documentElement.dataset.linkedRead=t,()=>document.documentElement.dataset.linkedRead='blocked');"));
    check.expect(waitUntil([&]{
        view.paneForTest(0)->page()->runJavaScript(QStringLiteral("document.documentElement.dataset.linkedRead||''"),
            [linked](const QVariant &value){*linked=value.toString();});
        return !linked->isEmpty();
    })&&*linked==QStringLiteral("blocked"),"local file symlink cannot escape the selected folder",*linked);
#endif
    const auto stylesheet=std::make_shared<QString>();
    view.paneForTest(0)->page()->runJavaScript(QStringLiteral("getComputedStyle(document.body).backgroundColor"),
        [stylesheet](const QVariant &value){*stylesheet=value.toString();});
    check.expect(waitUntil([&]{return !stylesheet->isEmpty();})&&*stylesheet==QStringLiteral("rgb(237, 244, 255)"),
        "relative local stylesheet still renders",*stylesheet);
    for(const QString &field:{QStringLiteral("workerRead"),QStringLiteral("blobRead")}){
        const auto reply=std::make_shared<QString>();
        const bool done=waitUntil([&]{
            view.paneForTest(0)->page()->runJavaScript(QStringLiteral("document.documentElement.dataset.%1||''").arg(field),
                [reply](const QVariant &value){*reply=value.toString();});
            return !reply->isEmpty();
        });
        const QJsonArray values=QJsonDocument::fromJson(reply->toUtf8()).array();
        check.expect(done&&values.size()==4&&values.at(0).toString()==QStringLiteral("blocked")
            &&values.at(1).toString()==QStringLiteral("{\"value\":\"local data\"}")
            &&values.at(2).toString()==QStringLiteral("blocked")&&values.at(3).toString()==QStringLiteral("blocked")
            &&*networkConnections==0,
            "file and blob workers deny raw file URLs and outbound requests while retaining relative JSON",field+QStringLiteral(": ")+*reply);
    }
    QStringList texts,images;
    QObject::connect(&view,&WebCompareView::openTextsRequested,&view,[&](const QStringList &paths){texts=paths;});
    QObject::connect(&view,&WebCompareView::openImagesRequested,&view,[&](const QStringList &paths){images=paths;});
    view.compareText(false);
    check.expect(waitUntil([&]{return texts.size()==2;})&&get(texts.value(0)).contains("Original text")
        &&get(texts.value(1)).contains("Changed text"),"rendered text snapshots retain both pages");
    texts.clear();view.compareText(true);
    check.expect(waitUntil([&]{return texts.size()==2;})&&get(texts.value(0)).contains("<h1>")
        &&get(texts.value(1)).contains("background:#fff1df"),"DOM HTML snapshots retain markup");
    bool changed=false;
    view.paneForTest(0)->page()->runJavaScript(QStringLiteral("document.getElementById('value').textContent='Live DOM update'; true"),[&](const QVariant&){changed=true;});
    waitUntil([&]{return changed;});texts.clear();view.compareText(false);
    check.expect(waitUntil([&]{return texts.size()==2;})&&get(texts.value(0)).contains("Live DOM update"),"text comparison uses current rendered DOM");
    view.compareScreenshots(false);
    check.expect(waitUntil([&]{return images.size()==2;}),"viewport screenshots complete");
    QImage leftImage(images.value(0)),rightImage(images.value(1));
    check.expect(!leftImage.isNull()&&!rightImage.isNull()&&leftImage.width()>100&&leftImage.height()>100
        &&leftImage!=rightImage,"rendered screenshots contain different page pixels");
    const auto reportScrollState=[&view](const char *phase){
        // Preserve the assertions below and report both APIs if a virtual
        // renderer stops publishing Qt scroll properties/signals. Shared
        // callback state remains valid even if a diagnostic reply is late.
        const auto replies=std::make_shared<int>(0);
        const QString label=QString::fromLatin1(phase);
        for(int i=0;i<2;++i){
            QWebEnginePage *page=view.paneForTest(i)->page();
            const QPointF position=page->scrollPosition();
            page->runJavaScript(QStringLiteral(
                "JSON.stringify({x:window.scrollX,y:window.scrollY,height:window.innerHeight,"
                "scrollHeight:document.documentElement.scrollHeight,visibility:document.visibilityState})"),
                [replies,label,i,position](const QVariant &value){
                    fprintf(stderr,"Web scroll diagnostic [%s] pane %d: Qt=(%.1f,%.1f), DOM=%s\n",
                        qPrintable(label),i,position.x(),position.y(),qPrintable(value.toString()));
                    ++*replies;
                });
        }
        waitUntil([replies]{return *replies==2;},1000);
    };
    view.paneForTest(0)->page()->runJavaScript(QStringLiteral("window.scrollTo(0,500)"));
    const bool initialSynced=waitUntil([&]{return view.paneForTest(1)->page()->scrollPosition().y()>100;},5000);
    check.expect(initialSynced,"scroll position synchronizes across pages");
    if(!initialSynced)reportScrollState("initial");

    // Read actual browser scroll ranges, independently of the application's
    // synchronization code. Fixtures deliberately have different heights.
    auto ranges=std::make_shared<QList<double>>(QList<double>{0.0,0.0});
    auto rangeResults=std::make_shared<int>(0);
    for(int i=0;i<2;++i)
        view.paneForTest(i)->page()->runJavaScript(
            QStringLiteral("Math.max(0,document.documentElement.scrollHeight-window.innerHeight)"),
            [ranges,rangeResults,i](const QVariant &result){(*ranges)[i]=result.toDouble();++*rangeResults;});
    check.expect(waitUntil([&]{return *rangeResults==2;})&&ranges->at(0)>900&&ranges->at(1)>900,
        "rendered pages expose scroll ranges for synchronization checks");
    const double ratio=ranges->at(1)/qMax(1.0,ranges->at(0));

    bool rapidSecondSent=false;
    const auto rapidConnection=QObject::connect(view.paneForTest(0)->page(),&QWebEnginePage::scrollPositionChanged,
        &view,[&](const QPointF &position){
            if(!rapidSecondSent&&qAbs(position.y()-250.0)<3.0){
                rapidSecondSent=true;
                view.paneForTest(0)->page()->runJavaScript(QStringLiteral("window.scrollTo(0,850)"));
            }
        });
    view.paneForTest(0)->page()->runJavaScript(QStringLiteral("window.scrollTo(0,250)"));
    const bool rapidSynced=waitUntil([&]{return rapidSecondSent
        &&qAbs(view.paneForTest(0)->page()->scrollPosition().y()-850.0)<3.0
        &&qAbs(view.paneForTest(1)->page()->scrollPosition().y()-850.0*ratio)<3.0;},5000);
    QObject::disconnect(rapidConnection);
    check.expect(rapidSynced,"rapid consecutive source scrolls retain the final position");
    if(!rapidSynced)reportScrollState(rapidSecondSent?"rapid-final":"rapid-first-event-missing");

    bool switchedSource=false;
    const auto switchConnection=QObject::connect(view.paneForTest(1)->page(),&QWebEnginePage::scrollPositionChanged,
        &view,[&](const QPointF &position){
            if(!switchedSource&&qAbs(position.y()-350.0*ratio)<3.0){
                switchedSource=true;
                view.paneForTest(1)->page()->runJavaScript(QStringLiteral("window.scrollTo(0,200)"));
            }
        });
    view.paneForTest(0)->page()->runJavaScript(QStringLiteral("window.scrollTo(0,350)"));
    const bool switchSynced=waitUntil([&]{return switchedSource
        &&qAbs(view.paneForTest(1)->page()->scrollPosition().y()-200.0)<3.0
        &&qAbs(view.paneForTest(0)->page()->scrollPosition().y()-200.0/qMax(0.001,ratio))<3.0;},5000);
    QObject::disconnect(switchConnection);
    check.expect(switchSynced,"scroll synchronization follows an immediate change of source pane");
    if(!switchSynced)reportScrollState(switchedSource?"source-switched":"source-switch-event-missing");

    view.paneForTest(0)->page()->runJavaScript(QStringLiteral("window.scrollTo(0,0)"));
    waitUntil([&]{return view.paneForTest(1)->page()->scrollPosition().y()<5;},5000);
    const QSize originalSize=view.size();
    images.clear();view.compareScreenshots(true);
    check.expect(waitUntil([&]{return images.size()==2;}),"full-page screenshots complete");
    const QImage fullLeft(images.value(0)),fullRight(images.value(1));
    check.expect(!fullLeft.isNull()&&!fullRight.isNull()&&fullLeft.height()>1500&&fullRight.height()>1700,
        "full-page captures include content beyond viewport");
    check.expect(view.size()==originalSize,"full-page capture restores the comparison window size",
        QStringLiteral("before %1x%2, after %3x%4").arg(originalSize.width()).arg(originalSize.height()).arg(view.width()).arg(view.height()));

    // Native navigation must retain independent origins and usable history.
    // A previously opened document must not acquire a newly selected folder.
    const QUrl originalPage=view.paneForTest(0)->url();
    const QString otherFolder=dir.filePath(QStringLiteral("other-pages"));
    const QString otherPage=QDir(otherFolder).filePath(QStringLiteral("next.html"));
    check.expect(QDir().mkpath(otherFolder)&&put(otherPage,"<!doctype html><p>Another selected folder</p>")
        &&put(QDir(otherFolder).filePath(QStringLiteral("data.json")),"other-folder-data"),
        "create a second local document root for navigation checks");
    auto *address=view.findChild<QLineEdit *>(QStringLiteral("webAddress0"));
    const auto navigateAddress=[&](const QString &text){
        loaded=false;success=false;
        if(!address)return false;
        address->setText(text);QMetaObject::invokeMethod(address,"returnPressed",Qt::DirectConnection);
        return waitUntil([&]{return loaded;})&&success;
    };
    check.expect(navigateAddress(otherPage),"the native address field opens a different local folder");
    const QUrl nextPage=view.paneForTest(0)->url();
    const auto scriptValue=[&](const QString &script){
        struct Reply {QString value;bool done=false;};
        const auto reply=std::make_shared<Reply>();
        view.paneForTest(0)->page()->runJavaScript(script,[reply](const QVariant &value){reply->value=value.toString();reply->done=true;});
        waitUntil([reply]{return reply->done;});
        return reply->value;
    };
    const auto readResource=[&](const QUrl &url){
        scriptValue(QStringLiteral("delete document.documentElement.dataset.historyRead;fetch(%1).then(r=>{if(!r.ok)throw Error('fetch');return r.text();}).then(t=>document.documentElement.dataset.historyRead=t,()=>document.documentElement.dataset.historyRead='blocked');")
            .arg(QString::fromUtf8(jsString(url.toString()))));
        QString result;
        waitUntil([&]{result=scriptValue(QStringLiteral("document.documentElement.dataset.historyRead||''"));return !result.isEmpty();});
        return result;
    };
    check.expect(readResource(QUrl(QStringLiteral("data.json")))==QStringLiteral("other-folder-data"),
        "new local selection reads data from its own folder");
    loaded=false;view.paneForTest(0)->back();
    check.expect(waitUntil([&]{return loaded;})&&success&&view.paneForTest(0)->url()==originalPage
        &&readResource(QUrl(QStringLiteral("data.json")))==QStringLiteral("{\"value\":\"local data\"}"),
        "Back restores the original document and its original folder");
    check.expect(readResource(nextPage.resolved(QUrl(QStringLiteral("data.json"))))==QStringLiteral("blocked"),
        "history does not grant the original document access to another selected folder");
    const QUrl peerPage=view.paneForTest(1)->url();
    check.expect(readResource(peerPage.resolved(QUrl(QStringLiteral("data.json"))))==QStringLiteral("blocked"),
        "a local pane cannot read another pane's local origin");
    const QString crossWorker=QStringLiteral(
        "(()=>{const w=new Worker('worker.js');w.onmessage=e=>{document.documentElement.dataset.crossWorker=JSON.stringify(e.data);w.terminate();};"
        "w.onerror=()=>document.documentElement.dataset.crossWorker='error';w.postMessage([%1,new URL('data.json',location.href).href]);})()")
        .arg(QString::fromUtf8(jsString(nextPage.resolved(QUrl(QStringLiteral("data.json"))).toString())));
    scriptValue(crossWorker);
    QString crossWorkerReply;
    const bool crossWorkerFinished=waitUntil([&]{crossWorkerReply=scriptValue(QStringLiteral("document.documentElement.dataset.crossWorker||''"));return !crossWorkerReply.isEmpty();});
    const QJsonArray crossWorkerValues=QJsonDocument::fromJson(crossWorkerReply.toUtf8()).array();
    check.expect(crossWorkerFinished&&crossWorkerValues.size()==2
        &&crossWorkerValues.at(0).toString()==QStringLiteral("blocked")
        &&crossWorkerValues.at(1).toString()==QStringLiteral("{\"value\":\"local data\"}"),
        "a restored worker retains its original folder boundary",crossWorkerReply);
    loaded=false;view.paneForTest(0)->forward();
    check.expect(waitUntil([&]{return loaded;})&&success&&view.paneForTest(0)->url()==nextPage
        &&readResource(QUrl(QStringLiteral("data.json")))==QStringLiteral("other-folder-data"),
        "Forward retains the selected document's separate folder");
    loaded=false;view.paneForTest(0)->back();
    const bool returned=waitUntil([&]{return loaded;})&&success;
    loaded=false;
    scriptValue(QStringLiteral("location.href='right.html';"));
    check.expect(returned&&waitUntil([&]{return loaded;})&&success
        &&QUrl(view.paths().value(0)).toLocalFile()==QFileInfo(second).canonicalFilePath(),
        "relative document navigation remains within the selected folder");

    // Automatic navigation must not silently turn a local document into a
    // network page. The native address field still allows an explicit URL.
    loaded=false;
    const QString remoteUrl=QStringLiteral("http://%1/native-navigation").arg(QString::fromLatin1(origin));
    scriptValue(QStringLiteral("location.href=%1;").arg(QString::fromUtf8(jsString(remoteUrl))));
    check.expect(waitUntil([&]{return loaded;})&&!success&&*networkConnections==0,
        "scripted local-to-network document navigation makes no connection",
        QStringLiteral("connections: %1").arg(*networkConnections));
    check.expect(navigateAddress(remoteUrl)&&*networkConnections>0,
        "the native address field can explicitly navigate from local content to HTTP");
    view.close();
    return check.finish("MergeDesk web");
}
}
