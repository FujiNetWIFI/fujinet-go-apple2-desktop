/*
 * MainWindow: menu bar over the emulator display for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>

#include "apple2session.h"

class DisplayWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(apple2session *session, QWidget *parent = nullptr);

private:
    void buildMenus();
    void applyDisplaySettings();
    void restartSession();
    void importDiskImage();
    void importSystemRoms();
    void status(const QString &message);

    apple2session *m_session;
    DisplayWidget *m_display;
};
