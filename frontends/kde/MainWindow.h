/*
 * MainWindow: menu bar over the emulator display for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>
#include <QTimer>

#include "apple2session.h"

class DisplayView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(apple2session *session, QWidget *parent = nullptr);

private:
    void buildMenus();
    void applyDisplaySettings();
    void restartSession();
    void useSoftwareDisplay();
    void importDiskImage();
    void importSystemRoms();
    void status(const QString &message);
    void toggleControllers();
    void showSettings(const QString &startTab = QString());
    /* the news the other threads leave: console actions and hot-plugs */
    void tick();
    void runSysaction(int sysact);
    void changeEvent(QEvent *event) override;

    apple2session *m_session;
    DisplayView *m_display;
    QTimer m_tickTimer;
    unsigned m_padGen = 0;
};
