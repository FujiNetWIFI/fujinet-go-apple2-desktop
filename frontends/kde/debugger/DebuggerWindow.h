/*
 * The KDE/Qt frontend's debugger window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>

#include <vector>

#include "apple2debug.h"
#include "apple2session.h"

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit DebuggerWindow(apple2session *session, QWidget *parent = nullptr);
    ~DebuggerWindow() override;

    /* One window per process; presents the existing one if there is one. */
    static void show(QWidget *parent, apple2session *session);

public slots:
    /* Invoked on the GUI thread. The engine's stop callback runs on the
     * EMULATOR thread and hands the event over with a queued connection --
     * nothing here may be called directly from that callback. */
    void onStopped(int reason, unsigned pc);

private:
    void buildUi();
    void renderRegs();
    void renderDisasm();
    void renderMem();
    void renderVideo();
    void refreshAll();
    void toggleRun();

    apple2session *m_session;
    apple2debug *m_dbg;

    QPushButton *m_run = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_regs = nullptr;
    QListWidget *m_disasm = nullptr;
    QLabel *m_mem = nullptr;
    QLineEdit *m_memAddr = nullptr;
    QComboBox *m_viewPick = nullptr;
    QLabel *m_viewPic = nullptr;
    std::vector<uint32_t> m_viewFb;

    quint16 m_disasmAt = 0;
    quint16 m_memAt = 0x0400;
    bool m_followPc = true;
};
