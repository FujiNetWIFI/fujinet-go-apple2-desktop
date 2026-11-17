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

#include "DisplayView.h"
#include "controllers/ControllersWindow.h"
#include "DisplayWidget.h"
#include "SoftDisplayWidget.h"
#include "debugger/DebuggerWindow.h"
#include "FujiNetWindows.h"
#include "help/HelpWindow.h"
#include "SettingsDialog.h"

#ifndef APPLE2_VERSION_STRING
#define APPLE2_VERSION_STRING "0.0.0"
#endif

MainWindow::MainWindow(apple2session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Apple II"));
    setProperty("apple2HelpContext", QStringLiteral("main"));
    resize(1120, 900);

    auto *gpu = new DisplayWidget(session, this);
    m_display = gpu;
    setCentralWidget(gpu);
    /* No usable GPU device: fall back to painting in software */
    connect(gpu, &QRhiWidget::renderFailed, this, &MainWindow::useSoftwareDisplay,
            Qt::QueuedConnection);
    statusBar();

    buildMenus();
    applyDisplaySettings();
    m_display->widget()->setFocus();

    /* One timer for the news the other threads leave: the gamepad thread
     * cannot call into Qt, so it posts console actions and hot-plug events and
     * this takes them. The baseline keeps pads already attached from
     * announcing themselves. */
    m_padGen = apple2session_gamepad_generation(session);
    connect(&m_tickTimer, &QTimer::timeout, this, &MainWindow::tick);
    m_tickTimer.start(50);

    /* Developer affordances: open the debugger, the Controllers window or the
     * settings alongside the main window, which is what you want when the app
     * dies before you reach a menu. */
    if (qEnvironmentVariableIsSet("APPLE2_OPEN_DEBUGGER"))
        DebuggerWindow::show(this, m_session);
    if (qEnvironmentVariableIsSet("APPLE2_OPEN_CONTROLLERS"))
        QTimer::singleShot(0, this, [this] { toggleControllers(); });
    if (qEnvironmentVariableIsSet("APPLE2_OPEN_PREFS"))
        QTimer::singleShot(0, this, [this] {
            showSettings(QString::fromLocal8Bit(qgetenv("APPLE2_OPEN_PREFS")));
        });
}

void MainWindow::toggleControllers()
{
    /* Hidden, not destroyed: a remap in progress and the window's position
     * both survive closing it. */
    apple2ControllersToggle(this, m_session);
}

void MainWindow::tick()
{
    const unsigned posted = apple2session_sysaction_take(m_session);
    for (int act = 0; act < APPLE2SESSION_SYSACT_COUNT; ++act)
        if (posted & (1u << act))
            runSysaction(act);

    const unsigned gen = apple2session_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        char msg[160];
        if (apple2session_gamepad_last_event(m_session, msg, sizeof msg) > 0)
            status(QString::fromUtf8(msg));
    }
}

/* A console action, from a key, a pad button, the Controllers window or the
 * menu: always here, on the UI thread. */
void MainWindow::runSysaction(int sysact)
{
    switch (sysact) {
    case APPLE2SESSION_SYSACT_RESET:
        apple2session_sysaction_fire(m_session, APPLE2SESSION_SYSACT_RESET);
        status(QStringLiteral("Reset"));
        break;
    case APPLE2SESSION_SYSACT_POWER:
        apple2session_sysaction_fire(m_session, APPLE2SESSION_SYSACT_POWER);
        status(QStringLiteral("Power cycled"));
        break;
    case APPLE2SESSION_SYSACT_DEBUG_STOP:
        apple2session_sysaction_fire(m_session, APPLE2SESSION_SYSACT_DEBUG_STOP);
        DebuggerWindow::showFor(this, m_session);
        break;
    default: break;
    }
}

/* Keys held into a focus change must not stay down. */
void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow() &&
        !apple2ControllersActive())
        apple2session_release_all(m_session);
}

/* A changed machine option restarts the session; the Input tab's changes apply
 * at once, and Open Controllers closes the dialog (as OK) so the window it
 * opens is not blocked by a modal one. */
void MainWindow::showSettings(const QString &startTab)
{
    SettingsDialog dlg(m_session, this, startTab);
    connect(&dlg, &SettingsDialog::displayChanged, this,
            &MainWindow::applyDisplaySettings);
    dlg.exec();
    apple2ControllersRefresh();
    if (dlg.machineDirty()) {
        restartSession();
        status(QStringLiteral("Machine options applied "
                              "(session restarted)"));
    }
    m_display->widget()->setFocus();
    if (dlg.openControllers())
        apple2ControllersShow(this, m_session);
}

void MainWindow::status(const QString &message)
{
    statusBar()->showMessage(message, 6000);
}

void MainWindow::applyDisplaySettings()
{
    m_display->setAspectMode(
        apple2session_get_int(m_session, "aspect_mode",
                              DisplayView::AspectTv43));
    m_display->setSmooth(
        apple2session_get_int(m_session, "smooth_scaling", 0));
}

void MainWindow::useSoftwareDisplay()
{
    if (dynamic_cast<SoftDisplayWidget *>(m_display))
        return;
    auto *soft = new SoftDisplayWidget(m_session, this);
    m_display = soft;
    setCentralWidget(soft);   /* deletes the GPU display, which stops its feed */
    applyDisplaySettings();
    soft->setFocus();
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
    machine->addAction(QStringLiteral("&Reset"), this,
                       [this] { runSysaction(APPLE2SESSION_SYSACT_RESET); });
    machine->addAction(QStringLiteral("&Power Cycle"), this,
                       [this] { runSysaction(APPLE2SESSION_SYSACT_POWER); });
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
                       [this] {
        if (apple2session_fujinet_external(m_session)) {
            status(QStringLiteral("FujiNet is external: use its own web UI"));
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fujinet->addAction(QStringLiteral("Console &Log…"), this,
                       [this] {
        if (apple2session_fujinet_external(m_session)) {
            status(QStringLiteral("FujiNet is external: see its own console"));
            return;
        }
        fujinet_log_show(this, m_session);
    });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Controllers"), QKeySequence(Qt::Key_F9),
                    this, [this] { toggleControllers(); });
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12),
                    this, [this] { DebuggerWindow::show(this, m_session); });

    QMenu *settings = menuBar()->addMenu(QStringLiteral("&Settings"));
    settings->addAction(QStringLiteral("&Settings…"), this,
                        [this] { showSettings(); });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    {
        /* works from every window of the app (the Debugger has the keyboard
         * too) */
        QAction *helpAct = help->addAction(QStringLiteral("FujiNet Go Apple II &Help"));
        helpAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
        helpAct->setShortcutContext(Qt::ApplicationShortcut);
        addAction(helpAct);
        connect(helpAct, &QAction::triggered, this, [this] {
            apple2HelpFor(QApplication::activeWindow() ? QApplication::activeWindow() : this);
        });
    }
    help->addSeparator();
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
