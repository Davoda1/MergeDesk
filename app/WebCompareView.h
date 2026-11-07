// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QWidget>
#include <QPointF>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

class QWebEngineView;
class QWebEngineProfile;
class QLineEdit;
class QLabel;
class QCheckBox;
class QTimer;
class WebLocalFiles;
class WebCompareView : public QWidget {
    Q_OBJECT
public:
    static void registerLocalScheme();
    explicit WebCompareView(const QStringList &urls,QWidget *parent=nullptr);
    ~WebCompareView() override;
    QStringList paths() const;
    QString tabTitle() const;
    void compareText(bool html=false);
    void compareScreenshots(bool fullPage=false);
    QWebEngineView *paneForTest(int pane) const;
signals:
    void openTextsRequested(const QStringList &paths);
    void openImagesRequested(const QStringList &paths);
    void titleChanged();
    void allPagesLoaded(bool success);
private:
    void navigate(int pane);
    bool ready();
    void syncScroll(int source,const QPointF &position);
    void readScrollPosition(int source);
    void flushScrollPosition();
    void resetScrollSync();
    struct ScrollState {
        bool reading=false;
        bool positionKnown=false;
        QPointF position;
        quint64 event=0;
    };
    QList<QWebEngineView*> m_views;
    QList<WebLocalFiles*> m_localFiles;
    QList<QLineEdit*> m_addresses;
    QList<bool> m_finished,m_success;
    QList<QWebEngineProfile*> m_profiles;
    QLabel *m_status=nullptr;
    QCheckBox *m_sync=nullptr;
    QTimer *m_scrollTimer=nullptr;
    QList<ScrollState> m_scrollStates;
    quint64 m_scrollEpoch=0,m_scrollEvent=0,m_latestSourceEvent=0;
    int m_pendingScrollSource=-1;
    QPointF m_pendingScrollPosition;
    double m_pendingScrollFraction=0;
    bool m_busy=false,m_destroying=false;
    std::unique_ptr<QTemporaryDir> m_temp;
    int m_serial=0;
};
