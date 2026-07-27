/*
 * FujiNet configuration window (embedded QtWebEngine view of the FujiNet
 * web admin at 127.0.0.1:8000, matching the Android app) and a console
 * log window streaming the runtime's captured output.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FujiNetWindows.h"

#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#ifdef HAVE_WEBENGINE
#include <QWebEngineView>
#else
#include <QDesktopServices>
#include <QUrl>
#endif

void fujinet_config_show(QWidget *parent, apple2session *session)
{
#ifdef HAVE_WEBENGINE
    static QPointer<QWidget> win;
    if (win) {
        win->raise();
        win->activateWindow();
        return;
    }
    win = new QWidget(parent, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("FujiNet Configuration"));
    win->resize(1000, 760);

    auto *view = new QWebEngineView(win);
    view->load(QUrl(QString::fromUtf8(apple2session_fujinet_webui_url(session))));
    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);
    win->show();
#else
    (void)parent;
    QDesktopServices::openUrl(
        QUrl(QString::fromUtf8(apple2session_fujinet_webui_url(session))));
#endif
}

void fujinet_log_show(QWidget *parent, apple2session *session)
{
    static QPointer<QWidget> win;
    if (win) {
        win->raise();
        win->activateWindow();
        return;
    }
    win = new QWidget(parent, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("FujiNet Console Log"));
    win->resize(820, 560);

    auto *view = new QPlainTextEdit(win);
    view->setReadOnly(true);
    QFont mono = view->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    view->setFont(mono);

    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);

    auto *timer = new QTimer(win);
    auto refresh = [view, session]() {
        static char buf[128 * 1024];
        const int n = apple2session_fujinet_copy_log(session, buf, sizeof(buf));
        view->setPlainText(n > 0 ? QString::fromUtf8(buf)
                                 : QStringLiteral("(no FujiNet output yet)"));
        view->verticalScrollBar()->setValue(
            view->verticalScrollBar()->maximum());
    };
    QObject::connect(timer, &QTimer::timeout, view, refresh);
    timer->start(1000);
    refresh();
    win->show();
}
