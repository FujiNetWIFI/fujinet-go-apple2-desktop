/*
 * FujiNet Go Apple II -- KDE/Qt frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>

#include <cstdio>

#include "MainWindow.h"
#include "apple2session.h"

int main(int argc, char *argv[])
{
    /* Vsync'd swaps for the display widget -- frameSwapped is what feeds the
     * session's phase-lock. Shared contexts for WebEngine. */
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("FujiNet Go Apple II"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.apple2.kde"));
#ifdef APPLE2_ICON_FILE
    /* Dev-tree fallback; installed runs resolve the themed icon. */
    app.setWindowIcon(QIcon::fromTheme(
        QStringLiteral("online.fujinet.go.apple2.kde"),
        QIcon(QStringLiteral(APPLE2_ICON_FILE))));
#endif

    apple2session *session = apple2session_new(nullptr);
    if (!session) {
        fprintf(stderr, "fatal: could not create the session\n");
        return 1;
    }
    apple2session_start_opts opts;
    apple2session_default_opts(session, &opts);
    if (apple2session_start(session, &opts) != 0)
        fprintf(stderr, "session start: %s\n",
                apple2session_last_error(session));

    MainWindow win(session);
    win.show();
    const int rc = app.exec();

    apple2session_free(session);
    return rc;
}
