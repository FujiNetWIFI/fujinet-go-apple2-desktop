/*
 * MainWindow: menu bar over the emulator display. Plain Qt6 Widgets, no KDE
 * Frameworks -- Breeze arrives through the platform theme, and staying
 * framework-free keeps this frontend reusable elsewhere.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

#include "DisplayWidget.h"
#include "debugger/DebuggerWindow.h"
#include "FujiNetWindows.h"
#include "SettingsDialog.h"

#ifndef APPLE2_VERSION_STRING
#define APPLE2_VERSION_STRING "0.0.0"
#endif

MainWindow::MainWindow(apple2session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Apple II"));
    resize(1120, 900);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);
    statusBar();

    buildMenus();
    applyDisplaySettings();
    m_display->setFocus();

    /* Developer affordance: open the debugger alongside the main window,
     * which is what you want when the app dies before you reach a menu. */
    if (qEnvironmentVariableIsSet("APPLE2_OPEN_DEBUGGER"))
        DebuggerWindow::show(this, m_session);
}

void MainWindow::status(const QString &message)
{
    statusBar()->showMessage(message, 6000);
}

void MainWindow::applyDisplaySettings()
{
    m_display->setAspectMode(
        apple2session_get_int(m_session, "aspect_mode",
                              DisplayWidget::AspectTv43));
    m_display->setSmooth(
        apple2session_get_int(m_session, "smooth_scaling", 0));
}

void MainWindow::restartSession()
{
    apple2session_start_opts opts;
    apple2session_stop(m_session);
    apple2session_default_opts(m_session, &opts);
    if (apple2session_start(m_session, &opts) != 0)
        status(QString::fromUtf8(apple2session_last_error(m_session)));
}

void MainWindow::importDiskImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Disk Image"), QString(),
        QStringLiteral("Apple II disk images (*.dsk *.do *.po *.2mg *.nib "
                       "*.woz *.hdv);;All files (*)"));
    if (path.isEmpty())
        return;

    char dest[1024];
    if (apple2session_import_media(m_session, path.toUtf8().constData(), dest,
                                   sizeof(dest)) != 0) {
        status(QString::fromUtf8(apple2session_last_error(m_session)));
        return;
    }
    status(QStringLiteral("Copied to the FujiNet SD folder. Mount it from "
                          "FujiNet Configuration."));
}

void MainWindow::importSystemRoms()
{
    /* AppleWin looks ROMs up by exact filename, so whatever is picked keeps
     * its name. Only a WITH_APPLE_ROMS=OFF build reads them. */
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Import System ROMs"), QString(),
        QStringLiteral("Apple II system ROMs (*.rom *.ROM *.bin);;"
                       "All files (*)"));
    if (paths.isEmpty())
        return;

    int ok = 0;
    for (const QString &path : paths) {
        char dest[1024];
        if (apple2session_import_rom(m_session, path.toUtf8().constData(),
                                     dest, sizeof(dest)) == 0)
            ok++;
    }
    if (ok == 0) {
        status(QString::fromUtf8(apple2session_last_error(m_session)));
        return;
    }
    restartSession();
    status(QStringLiteral("Imported %1 ROM file%2 (session restarted)")
               .arg(ok)
               .arg(ok == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("&Reset"), this, [this] {
        apple2session_reset(m_session, 0);
        status(QStringLiteral("Reset"));
    });
    machine->addAction(QStringLiteral("&Power Cycle"), this, [this] {
        apple2session_reset(m_session, 1);
        status(QStringLiteral("Power cycled"));
    });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, qApp,
                       &QApplication::quit);

    QMenu *media = menuBar()->addMenu(QStringLiteral("Me&dia"));
    media->addAction(QStringLiteral("Import &Disk Image…"), this,
                     &MainWindow::importDiskImage);
    media->addAction(QStringLiteral("Import System &ROMs…"), this,
                     &MainWindow::importSystemRoms);

    QMenu *fujinet = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fujinet->addAction(QStringLiteral("&Configuration…"), this,
                       [this] { fujinet_config_show(this, m_session); });
    fujinet->addAction(QStringLiteral("Console &Log…"), this,
                       [this] { fujinet_log_show(this, m_session); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12),
                    this, [this] { DebuggerWindow::show(this, m_session); });

    QMenu *settings = menuBar()->addMenu(QStringLiteral("&Settings"));
    settings->addAction(QStringLiteral("&Settings…"), this, [this] {
        SettingsDialog dlg(m_session, this);
        connect(&dlg, &SettingsDialog::displayChanged, this,
                &MainWindow::applyDisplaySettings);
        dlg.exec();
        if (dlg.machineDirty()) {
            restartSession();
            status(QStringLiteral("Machine options applied "
                                  "(session restarted)"));
        }
        m_display->setFocus();
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About"), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("FujiNet Go Apple II"),
            QStringLiteral(
                "<b>FujiNet Go Apple II</b> %1<br><br>"
                "Self-contained Apple II with built-in FujiNet.<br>"
                "Emulation by AppleWin; FujiNet runs in-process over "
                "SmartPort-over-SLIP.<br><br>"
                "Copyright © 2026 Thomas Cherryhomes<br>"
                "GPL-3.0-or-later &middot; "
                "<a href=\"https://fujinet.online/\">fujinet.online</a>")
                .arg(QStringLiteral(APPLE2_VERSION_STRING)));
    });
}
