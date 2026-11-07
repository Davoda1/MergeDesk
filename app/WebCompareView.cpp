// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "WebCompareView.h"
#include "Dialogs.h"
#include "SafeFileOps.h"
#include <QAction>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineSettings>
#include <QWebEngineHistory>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QToolBar>
#include <QToolButton>
#include <QLineEdit>
#include <QLabel>
#include <QCheckBox>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QMimeDatabase>
#include <QUuid>
#include <QPixmap>
#include <QScrollArea>
#include <QMutex>
#include <QMutexLocker>
#include <memory>
#include <functional>
#include <cmath>

namespace {
const QByteArray localSchemeName("mergedesk-local");

bool isLocalPageOrigin(const QUrl &url)
{
    if(url.scheme()==QString::fromLatin1(localSchemeName))return true;
    return url.scheme()==QStringLiteral("blob")
        &&QUrl(url.path()).scheme()==QString::fromLatin1(localSchemeName);
}

class WebRequestPolicy final : public QWebEngineUrlRequestInterceptor {
public:
    explicit WebRequestPolicy(QObject *parent):QWebEngineUrlRequestInterceptor(parent){}
    void interceptRequest(QWebEngineUrlRequestInfo &request) override {
        const QUrl url=request.requestUrl();
        if(url.isLocalFile()){
            // Local documents use the checked scheme handler. Never grant a
            // renderer raw file access, including through a UNC URL or worker.
            request.block(true);return;
        }
        if(isLocalPageOrigin(request.firstPartyUrl())||isLocalPageOrigin(request.initiator())){
            const QString scheme=url.scheme();
            // Use the request's origin rather than the pane's current address:
            // an older document/worker keeps its restrictions during navigation.
            request.block(scheme!=QString::fromLatin1(localSchemeName)
                &&scheme!=QStringLiteral("data")&&scheme!=QStringLiteral("blob")
                &&scheme!=QStringLiteral("about"));
        }
    }
};
}

class WebLocalFiles final : public QWebEngineUrlSchemeHandler {
public:
    explicit WebLocalFiles(QObject *parent):QWebEngineUrlSchemeHandler(parent){}

    QUrl addDocument(const QUrl &fileUrl) {
        // Reject remote file hosts before any filesystem lookup.
        if(!fileUrl.isLocalFile()||!fileUrl.host().isEmpty())return {};
        const QFileInfo document(fileUrl.toLocalFile());
        const QString canonical=document.canonicalFilePath();
        if(!document.isFile()||canonical.isEmpty())return {};
        QString root=QFileInfo(canonical).absolutePath();
        if(!root.endsWith(QLatin1Char('/')))root+=QLatin1Char('/');
        QUrl url;
        url.setScheme(QString::fromLatin1(localSchemeName));
        url.setPath(QLatin1Char('/')+QFileInfo(canonical).fileName());
        url.setQuery(fileUrl.query());url.setFragment(fileUrl.fragment());
        QMutexLocker lock(&m_mutex);
        QString host;
        do{host=QUuid::createUuid().toString(QUuid::Id128);}while(m_roots.contains(host));
        // A host is never rebound to another folder. Retaining old bindings
        // preserves Back/Forward without granting old workers a new root.
        m_roots.insert(host,root);url.setHost(host);
        return url;
    }

    QUrl displayedUrl(const QUrl &url) const {
        if(url.scheme()!=QString::fromLatin1(localSchemeName))return url;
        QString root;
        {QMutexLocker lock(&m_mutex);root=m_roots.value(url.host());}
        if(root.isEmpty())return {};
        QUrl fileUrl=QUrl::fromLocalFile(QDir(root).filePath(url.path(QUrl::FullyDecoded).mid(1)));
        fileUrl.setQuery(url.query());fileUrl.setFragment(url.fragment());
        return fileUrl;
    }

    void requestStarted(QWebEngineUrlRequestJob *job) override {
        const QUrl url=job->requestUrl(),origin=job->initiator();
        QString root;
        {QMutexLocker lock(&m_mutex);root=m_roots.value(url.host());}
        if(url.scheme()!=QString::fromLatin1(localSchemeName)||root.isEmpty()
            ||!url.userInfo().isEmpty()||url.port()!=-1||job->requestMethod()!=QByteArrayLiteral("GET")){
            job->fail(QWebEngineUrlRequestJob::RequestDenied);return;
        }
        // Empty means native navigation. The opaque "null" origin is not
        // empty and must not gain access to a known folder or history entry.
        if(!origin.isEmpty()&&(origin.scheme()!=url.scheme()||origin.host()!=url.host()
            ||origin.port()!=url.port()||!origin.userInfo().isEmpty())){
            job->fail(QWebEngineUrlRequestJob::RequestDenied);return;
        }
        QString relative=url.path(QUrl::FullyDecoded);
        if(!relative.startsWith(QLatin1Char('/'))||relative.startsWith(QStringLiteral("//"))
            ||relative.contains(QChar(0))){
            job->fail(QWebEngineUrlRequestJob::RequestDenied);return;
        }
        relative.remove(0,1);
#ifdef Q_OS_WIN
        // Alternate separators and drive/device/stream syntax are not paths
        // relative to the selected folder. They are valid POSIX filenames.
        if(relative.contains(QLatin1Char('\\'))||relative.contains(QLatin1Char(':'))){
            job->fail(QWebEngineUrlRequestJob::RequestDenied);return;
        }
#endif
        const QStringList components=relative.split(QLatin1Char('/'));
        if(components.contains(QStringLiteral(".."))||components.contains(QStringLiteral("."))){
            job->fail(QWebEngineUrlRequestJob::RequestDenied);return;
        }
        const QFileInfo requested(QDir(root).filePath(relative));
        const QString canonical=requested.canonicalFilePath();
        // Canonicalize both file and directory links, and open that exact
        // canonical path. Keep case significant for case-sensitive volumes.
        if(!requested.isFile()||canonical.isEmpty()||!canonical.startsWith(root)){
            job->fail(QWebEngineUrlRequestJob::RequestDenied);return;
        }
        auto *file=new QFile(canonical,job);
        if(!file->open(QIODevice::ReadOnly)){
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);return;
        }
        // Worker scripts receive their own response policy; blob workers
        // inherit their creator's. Inline scripts and local assets still work.
        job->setAdditionalResponseHeaders({{QByteArrayLiteral("Content-Security-Policy"),
            QByteArrayLiteral("default-src 'self' data: blob:; connect-src 'self' data: blob:; "
                "script-src 'self' 'unsafe-inline' 'unsafe-eval' data: blob:; "
                "style-src 'self' 'unsafe-inline' data: blob:; worker-src 'self' data: blob:; "
                "base-uri 'self'; form-action 'self'")}});
        // Chromium can consume the device after this method returns. The job
        // owns it until the response is finished or cancelled.
        job->reply(QMimeDatabase().mimeTypeForFile(canonical).name().toUtf8(),file);
    }
private:
    mutable QMutex m_mutex;
    QHash<QString,QString> m_roots;
};

void WebCompareView::registerLocalScheme()
{
    QWebEngineUrlScheme scheme(localSchemeName);
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    // This is a regular origin with fetch/worker support, without the
    // LocalAccessAllowed flag that would grant access to file:// resources.
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme|QWebEngineUrlScheme::CorsEnabled
        |QWebEngineUrlScheme::FetchApiAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);
}

WebCompareView::WebCompareView(const QStringList &urls,QWidget *parent):QWidget(parent),m_temp(std::make_unique<QTemporaryDir>())
{
    setObjectName(QStringLiteral("webCompareView"));
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto *bar=new QToolBar(this);bar->setObjectName(QStringLiteral("webCompareToolbar"));
    bar->addAction(tr("Compare text"),this,[this]{compareText(false);});
    bar->addAction(tr("Compare HTML"),this,[this]{compareText(true);});
    bar->addSeparator();
    const auto zoomAction=[bar](const QString &text,const QString &description,const std::function<void()> &apply) {
        QAction *action=bar->addAction(text,bar,apply);
        action->setToolTip(description);
        if(auto *button=qobject_cast<QToolButton *>(bar->widgetForAction(action)))
            button->setAccessibleName(description);
    };
    zoomAction(QStringLiteral("−"),tr("Zoom out"),[this]{for(auto *v:m_views)v->setZoomFactor(qMax(0.25,v->zoomFactor()-0.1));});
    zoomAction(tr("100%"),tr("Actual size"),[this]{for(auto *v:m_views)v->setZoomFactor(1.0);});
    zoomAction(QStringLiteral("+"),tr("Zoom in"),[this]{for(auto *v:m_views)v->setZoomFactor(qMin(5.0,v->zoomFactor()+0.1));});
    layout->addWidget(bar);
    // Two native rows keep the translated controls within a 680 px window.
    // The checkbox lives outside the toolbar so it cannot disappear into
    // Qt's action-only overflow menu when the user increases the font size.
    auto *captureRow=new QWidget(this);
    auto *captureLayout=new QHBoxLayout(captureRow);
    captureLayout->setContentsMargins(0,0,6,0);captureLayout->setSpacing(6);
    auto *captureBar=new QToolBar(captureRow);captureBar->setObjectName(QStringLiteral("webCompareCaptureToolbar"));
    captureBar->addAction(tr("Compare screenshots"),this,[this]{compareScreenshots(false);});
    captureBar->addAction(tr("Full pages"),this,[this]{compareScreenshots(true);});
    captureLayout->addWidget(captureBar,1);
    m_sync=new QCheckBox(tr("Sync scroll"),captureRow);m_sync->setObjectName(QStringLiteral("webSyncScroll"));m_sync->setChecked(true);
    captureLayout->addWidget(m_sync);
    layout->addWidget(captureRow);
    m_scrollTimer=new QTimer(this);
    m_scrollTimer->setSingleShot(true);m_scrollTimer->setInterval(16);
    connect(m_scrollTimer,&QTimer::timeout,this,&WebCompareView::flushScrollPosition);
    connect(m_sync,&QCheckBox::toggled,this,[this]{resetScrollSync();});
    auto *split=new QSplitter(Qt::Horizontal,this);split->setChildrenCollapsible(false);layout->addWidget(split,1);
    const int count=qBound(2,int(urls.size()),3);
    for(int i=0;i<count;++i) {
        auto *container=new QWidget(split);auto *column=new QVBoxLayout(container);
        column->setContentsMargins(3,3,3,3);column->setSpacing(4);
        auto *row=new QHBoxLayout;
        auto *back=new QToolButton(container);back->setText(QStringLiteral("←"));back->setToolTip(tr("Back"));
        auto *forward=new QToolButton(container);forward->setText(QStringLiteral("→"));forward->setToolTip(tr("Forward"));
        auto *reload=new QToolButton(container);reload->setText(QStringLiteral("↻"));reload->setToolTip(tr("Reload"));
        auto *address=new QLineEdit(urls.value(i),container);address->setPlaceholderText(tr("URL or local HTML file"));
        back->setAccessibleName(tr("Back"));forward->setAccessibleName(tr("Forward"));reload->setAccessibleName(tr("Reload"));
        address->setAccessibleName(tr("URL or local HTML file"));
        address->setObjectName(QStringLiteral("webAddress%1").arg(i));
        row->addWidget(back);row->addWidget(forward);row->addWidget(reload);row->addWidget(address,1);column->addLayout(row);
        auto *view=new QWebEngineView(container);view->setMinimumWidth(150);
        // Each pane has an ephemeral profile, so web sessions and folder
        // capabilities are never shared with another compared page.
        auto *profile=new QWebEngineProfile(this);
        profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
        profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
        auto *policy=new WebRequestPolicy(profile);
        profile->setUrlRequestInterceptor(policy);
        auto *localFiles=new WebLocalFiles(profile);
        profile->installUrlSchemeHandler(localSchemeName,localFiles);
        m_profiles.append(profile);
        m_localFiles.append(localFiles);
        view->setPage(new QWebEnginePage(profile,view));
        view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls,false);
        view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls,false);
        view->settings()->setAttribute(QWebEngineSettings::DnsPrefetchEnabled,false);
        view->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows,false);
        view->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled,false);
        column->addWidget(view,1);split->addWidget(container);
        m_views.append(view);m_addresses.append(address);m_finished.append(false);m_success.append(false);
        m_scrollStates.append(ScrollState{});
        connect(address,&QLineEdit::returnPressed,this,[this,i]{navigate(i);});
        connect(back,&QToolButton::clicked,view,&QWebEngineView::back);
        connect(forward,&QToolButton::clicked,view,&QWebEngineView::forward);
        connect(reload,&QToolButton::clicked,view,&QWebEngineView::reload);
        connect(view,&QWebEngineView::loadStarted,this,[this,i]{m_finished[i]=false;m_success[i]=false;resetScrollSync();m_status->setText(tr("Loading webpages…"));});
        connect(view,&QWebEngineView::loadFinished,this,[this,i](bool ok){
            m_finished[i]=true;m_success[i]=ok;
            bool finished=true,success=true;for(int j=0;j<m_finished.size();++j){finished &=m_finished[j];success &=m_success[j];}
            if(finished){m_status->setText(success?tr("Pages loaded. Compare their rendered screenshots, text, or HTML."):tr("A page could not be loaded. Check its address and connection."));emit allPagesLoaded(success);}
        });
        connect(view,&QWebEngineView::urlChanged,this,[this,i](const QUrl &u){m_addresses[i]->setText(m_localFiles[i]->displayedUrl(u).toDisplayString());emit titleChanged();});
        connect(view->page(),&QWebEnginePage::scrollPositionChanged,this,[this,i](const QPointF &p){syncScroll(i,p);});
        connect(view->page(),&QWebEnginePage::permissionRequested,this,[](const QWebEnginePermission &permission){
            // Comparing a page does not grant it access to a microphone, camera or location.
            permission.deny();
        });
    }
    m_status=new QLabel(tr("Enter two or three webpage addresses."),this);m_status->setWordWrap(true);m_status->setContentsMargins(8,5,8,5);layout->addWidget(m_status);
    QTimer::singleShot(0,this,[this]{for(int i=0;i<m_views.size();++i)navigate(i);});
}

WebCompareView::~WebCompareView()
{
    // Chromium requires every page to be destroyed before its profile and
    // profile-owned interceptor, including any worker request callbacks.
    m_destroying=true;
    for(auto *view:m_views){
        disconnect(view,nullptr,this,nullptr);
        disconnect(view->page(),nullptr,this,nullptr);
        delete view;
    }
    m_views.clear();
    m_localFiles.clear();
    for(auto *profile:m_profiles)delete profile;
    m_profiles.clear();
}

void WebCompareView::navigate(int pane)
{
    const QString input=m_addresses[pane]->text().trimmed();
    if(input.isEmpty()){m_finished[pane]=true;m_success[pane]=false;return;}
    QUrl url=QFileInfo(input).isFile()?QUrl::fromLocalFile(QFileInfo(input).absoluteFilePath()):QUrl::fromUserInput(input);
    if(!QStringList{QStringLiteral("http"),QStringLiteral("https"),QStringLiteral("file")}.contains(url.scheme())){
        m_status->setText(tr("Use an HTTP, HTTPS, or local file address."));return;
    }
    if(url.isLocalFile()){
        url=m_localFiles[pane]->addDocument(url);
        if(url.isEmpty()){
            m_status->setText(tr("A page could not be loaded. Check its address and connection."));return;
        }
    }
    m_views[pane]->setUrl(url);
}
QStringList WebCompareView::paths() const {QStringList out;for(auto *a:m_addresses)out.append(a->text());return out;}
QString WebCompareView::tabTitle() const {return tr("Webpage comparison");}
QWebEngineView *WebCompareView::paneForTest(int pane) const {return m_views.value(pane);}
bool WebCompareView::ready()
{
    if(m_busy)return false;
    for(bool ok:m_success)if(!ok){m_status->setText(tr("Wait for all pages to load successfully before comparing."));return false;}
    return m_temp->isValid();
}
void WebCompareView::syncScroll(int source,const QPointF &position)
{
    Q_UNUSED(position);
    if(m_destroying || !m_sync->isChecked() || source<0 || source>=m_views.size()
        || !m_success.value(source))return;
    // Retain the newest event even while a previous renderer request is in
    // flight. A time-based global guard loses the user's final scroll and
    // blocks changing the source pane during that guard.
    m_scrollStates[source].event=++m_scrollEvent;
    if(!m_scrollStates[source].reading)readScrollPosition(source);
}

void WebCompareView::resetScrollSync()
{
    ++m_scrollEpoch;
    m_scrollTimer->stop();m_pendingScrollSource=-1;
    m_latestSourceEvent=m_scrollEvent;
    for(int i=0;i<m_views.size();++i){
        m_scrollStates[i]=ScrollState{};
        m_views[i]->page()->runJavaScript(
            QStringLiteral("delete globalThis.__mergedeskScrollSyncPosition;"),
            QWebEngineScript::ApplicationWorld);
    }
}

void WebCompareView::readScrollPosition(int source)
{
    if(m_destroying || !m_sync->isChecked())return;
    ScrollState &state=m_scrollStates[source];
    state.reading=true;
    const quint64 event=state.event,epoch=m_scrollEpoch;
    const QPointer<WebCompareView> guard(this);
    // The marker lives in Qt's isolated application world, not the page's
    // JavaScript globals. It suppresses only a position that we actually
    // requested. A wheel/key/script scroll to another position becomes a
    // new source immediately. Keep matching markers across coalesced events;
    // a delayed notification can describe the same programmatic position.
    const QString script=QStringLiteral(R"JS((()=>{
        const root=document.scrollingElement||document.documentElement;
        const x=window.scrollX,y=window.scrollY;
        const marker=globalThis.__mergedeskScrollSyncPosition;
        const feedback=!!marker&&Math.abs(marker.x-x)<0.5&&Math.abs(marker.y-y)<0.5;
        if(marker&&!feedback)delete globalThis.__mergedeskScrollSyncPosition;
        return [x,y,Math.max(0,root.scrollHeight-root.clientHeight),feedback];
    })())JS");
    m_views[source]->page()->runJavaScript(script,QWebEngineScript::ApplicationWorld,
        [guard,source,event,epoch](const QVariant &result){
        if(!guard || guard->m_destroying || epoch!=guard->m_scrollEpoch)return;
        ScrollState &current=guard->m_scrollStates[source];
        current.reading=false;
        if(!guard->m_sync->isChecked())return;
        if(event!=current.event){
            guard->readScrollPosition(source);
            return;
        }
        const QVariantList values=result.toList();
        if(values.size()!=4)return;
        const QPointF position(values[0].toDouble(),values[1].toDouble());
        const double extent=values[2].toDouble();
        if(!std::isfinite(position.x()) || !std::isfinite(position.y()) || !std::isfinite(extent))return;
        if(values[3].toBool()){
            current.position=position;current.positionKnown=true;
            return;
        }
        // An older reply from another renderer must not replace a more
        // recent source. Do not cache that stale sample: a newer signal from
        // the same pane still needs to be considered on its own merits.
        if(event<guard->m_latestSourceEvent)return;
        if(current.positionKnown && current.position==position)return;
        current.position=position;current.positionKnown=true;
        guard->m_latestSourceEvent=event;
        guard->m_pendingScrollSource=source;
        guard->m_pendingScrollPosition=position;
        guard->m_pendingScrollFraction=extent>0?qBound(0.0,position.y()/extent,1.0):0;
        if(!guard->m_scrollTimer->isActive())guard->m_scrollTimer->start();
    });
}

void WebCompareView::flushScrollPosition()
{
    const int source=m_pendingScrollSource;
    m_pendingScrollSource=-1;
    if(m_destroying || !m_sync->isChecked() || source<0 || source>=m_views.size())return;
    // CSS pixel coordinates and the scrolling element's actual viewport
    // avoid rounding/zoom errors from mixing QWidget and DOM dimensions.
    // Instant behavior also prevents a page's smooth-scroll CSS from
    // generating an animation of apparent new source positions.
    const QString script=QStringLiteral(R"JS((()=>{
        const root=document.scrollingElement||document.documentElement;
        window.scrollTo({left:%1,top:%2*Math.max(0,root.scrollHeight-root.clientHeight),behavior:'instant'});
        globalThis.__mergedeskScrollSyncPosition={x:window.scrollX,y:window.scrollY};
    })())JS")
        .arg(QString::number(m_pendingScrollPosition.x(),'g',17),
            QString::number(m_pendingScrollFraction,'g',17));
    for(int i=0;i<m_views.size();++i)if(i!=source && m_success.value(i))
        m_views[i]->page()->runJavaScript(script,QWebEngineScript::ApplicationWorld);
}
void WebCompareView::compareText(bool html)
{
    if(!ready())return;
    m_busy=true;
    struct State {QStringList paths;int completed=0;QString error;};
    auto state=std::make_shared<State>();for(int i=0;i<m_views.size();++i)state->paths.append(QString());
    const int serial=++m_serial;
    const QPointer<WebCompareView> guard(this);
    for(int i=0;i<m_views.size();++i){
        auto callback=[guard,state,i,serial,html](const QString &text){
            if(!guard || guard->m_destroying)return;
            const QString path=guard->m_temp->filePath(QStringLiteral("page-%1-%2.%3").arg(serial).arg(i+1).arg(html?QStringLiteral("html"):QStringLiteral("txt")));
            if(md::writeBytesAtomic(path,text.toUtf8(),&state->error))state->paths[i]=path;
            if(++state->completed==guard->m_views.size()){
                guard->m_busy=false;
                if(!state->error.isEmpty())lm::warning(guard,tr("Webpage comparison"),state->error);
                else emit guard->openTextsRequested(state->paths);
            }
        };
        if(html)m_views[i]->page()->toHtml(callback);else m_views[i]->page()->toPlainText(callback);
    }
}
void WebCompareView::compareScreenshots(bool fullPage)
{
    if(!ready())return;
    m_busy=true;
    QList<QSize> oldSizes,oldMinimums,oldMaximums;
    const QRect oldWindowGeometry=window()->geometry();
    const QSize oldWindowMinimum=window()->minimumSize();
    const bool wasMaximized=window()->isMaximized();
    qint64 totalPixels=0;
    for(auto *v:m_views){
        oldSizes.append(v->size());
        oldMinimums.append(v->minimumSize());oldMaximums.append(v->maximumSize());
        if(fullPage){
            const QSize contents=v->page()->contentsSize().toSize().expandedTo(v->size());
            const qreal scale=v->devicePixelRatioF();
            totalPixels+=qint64(contents.width()*scale)*qint64(contents.height()*scale);
            if(contents.height()*scale>16384 || contents.width()*scale>16384 || totalPixels>48*1024*1024){
                m_busy=false;m_status->setText(tr("This page is too large for one screenshot. Use Compare screenshots for the visible area."));return;
            }
        }
    }
    if(fullPage)for(auto *v:m_views)v->setFixedSize(v->page()->contentsSize().toSize().expandedTo(v->size()));
    QTimer::singleShot(350,this,[this,oldSizes,oldMinimums,oldMaximums,fullPage,oldWindowGeometry,oldWindowMinimum,wasMaximized]{
        QStringList files;QString error;const int serial=++m_serial;
        for(int i=0;i<m_views.size();++i){
            const QString path=m_temp->filePath(QStringLiteral("screenshot-%1-%2.png").arg(serial).arg(i+1));
            if(m_views[i]->grab().save(path,"PNG"))files.append(path);else error=tr("Could not capture the rendered page.");
            if(fullPage){
                auto *view=m_views[i];
                view->setMinimumSize(oldMinimums[i]);view->setMaximumSize(oldMaximums[i]);view->resize(oldSizes[i]);
                // QSplitter caches its children's minimum-size hints. Restore
                // the inner layouts first, before the enclosing window is
                // allowed to calculate its own minimum size again.
                auto *container=view->parentWidget();
                container->layout()->invalidate();container->layout()->activate();
                container->updateGeometry();container->parentWidget()->updateGeometry();
            }
        }
        if(fullPage){
            // Let the posted layout requests propagate through the splitter
            // and tab widget before restoring the user's window geometry.
            // Emitting the result after restoration also prevents opening the
            // image-comparison tab while the temporary constraints remain.
            QTimer::singleShot(0,this,[this,oldWindowGeometry,oldWindowMinimum,wasMaximized,files,error]{
                if(layout()){layout()->invalidate();layout()->activate();}
                if(window()->layout()){window()->layout()->invalidate();window()->layout()->activate();}
                window()->setMinimumSize(oldWindowMinimum);
                window()->setGeometry(oldWindowGeometry);
                if(wasMaximized)window()->showMaximized();
                m_busy=false;
                if(!error.isEmpty())lm::warning(this,tr("Webpage comparison"),error);else emit openImagesRequested(files);
            });
            return;
        }
        m_busy=false;
        if(!error.isEmpty())lm::warning(this,tr("Webpage comparison"),error);else emit openImagesRequested(files);
    });
}
