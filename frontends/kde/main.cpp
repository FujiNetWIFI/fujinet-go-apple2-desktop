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

#include "Accent.h"
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

    /* Toggled-on push buttons (Map while armed, the chosen side of a segment)
     * wear the accent colour with dark text, light or dark. The rule spells out
     * the whole box: with only a background colour the style still draws its
     * own bevel, which darkens it (Breeze), lightens it (Fusion) or ignores it
     * (Kvantum). */
    const QColor accent = apple2CtlAccent();
    app.setStyleSheet(
        QStringLiteral(
            "QPushButton[accent=\"true\"] { background-color: %1; color: %4;"
            " border: 1px solid %3; border-radius: 4px; padding: 4px 12px; }"
            "QPushButton[accent=\"true\"]:hover { background-color: %2; }")
            .arg(accent.name(), accent.lighter(112).name(), accent.darker(140).name(),
                 apple2CtlAccentInk().name()));

    int rc;
    {
        /* The windows go before the session does: the Controllers window still
         * calls into it as it is hidden. */
        MainWindow win(session);
        win.show();
        rc = app.exec();
    }

    apple2session_free(session);
    return rc;
}
