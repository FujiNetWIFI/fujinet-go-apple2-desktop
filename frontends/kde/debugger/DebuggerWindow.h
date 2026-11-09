/*
 * Debugger window (Qt6 Widgets) over the 6502 debugger engine, via
 * core/include/apple2debug.h. Mirrors the GNOME one tab for tab, and follows
 * the astrocade, SMS, MSX, Atari 7800 and ADAM KDE debuggers' design: a
 * QTabWidget under a toolbar.
 *
 * The engine is attached while the window is shown (which stops the
 * machine, as on every sibling) and detached when it is hidden, which lets
 * the machine run on. The window is never deleted by its close button: it
 * is hidden, and comes back as it was.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QImage>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <vector>

#include "apple2debug.h"
#include "apple2session.h"

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, apple2session *session);
    /* F12: shows it, or hides it (detaching) when it is already up. */
    static void toggleFor(QWidget *parent, apple2session *session);
    /* The entry point MainWindow has always used: the same as showFor. */
    static void show(QWidget *parent, apple2session *session);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(apple2session *session, QWidget *parent);
    ~DebuggerWindow() override;
    QWidget *buildToolbar();
    QWidget *buildConsole();
    QWidget *buildRegs();
    QWidget *buildDisasm();
    QWidget *buildMemory();
    QWidget *buildSearchBar();
    QWidget *buildVideo();
    QWidget *buildBreaks();
    QWidget *buildWatches();
    QWidget *buildSlots();
    QWidget *buildHist(int idx);
    QWidget *buildFujinet();

    void refreshStatus();
    void refreshCpu();
    void refreshMem();
    void refreshDisasm();
    void refreshVideo();
    void refreshBps();
    void refreshWatches(bool force);
    void refreshSlots();
    void refreshHist(int idx, bool force);
    void refreshFujinet(bool force);
    void refreshPage(int page, bool live);
    void refreshAll();
    void resync();
    void syncLater();
    void settleAndSync();
    void tick();

    void consoleAppend(const QString &text);
    void runConsole();
    void completeConsole();
    void say(const QString &msg);
    void message(const QString &msg);

    void toggleRun();
    void stepAnd(const std::function<void()> &fn);
    void runToCursor();
    void toggleBreakpoint(int addr);
    void toggleSelected();
    void addBreakpoint();
    void setReg(int i);
    void setFlag(int id, bool on);
    void memGoto();
    void memSearch(bool backward);
    void showInMemory(uint16_t lo, uint16_t hi);
    void popOut(int tab);
    void dock(int tab);
    int currentTab() const;
    bool tabVisible(int tab) const;
    void addWatch();
    void watchBreak(int index);
    void memWrite();
    void jumpTo();
    void loadSymbols();
    void save(const char *kind, const QString &title, const QString &file);
    int visibleRows() const;
    bool numberOf(const QString &text, long *out, QString *why = nullptr);
    bool addressOf(const QString &text, long *out, QString *why = nullptr);
    void setEntry(QLineEdit *e, const QString &text);

    apple2session *m_session;
    apple2debug *m_dbg;
    bool m_attached = false;            /* by this window: the engine may be gone at exit */
    unsigned m_seenGen = 0;
    unsigned m_seenStops = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    /* registers strip: PC SP A X Y P */
    QLineEdit *m_reg[6] = {};
    QCheckBox *m_flag[7] = {};          /* N V B D I Z C */
    QCheckBox *m_flagRo = nullptr;      /* bit 5: always 1, shown not set */
    QLabel *m_beam = nullptr;
    QPlainTextEdit *m_mem = nullptr;
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    int m_memTop = 0;
    bool m_hl = false;                  /* a range is highlighted in the dump */
    uint16_t m_hlLo = 0, m_hlHi = 0;
    QComboBox *m_srchKind = nullptr;
    QLineEdit *m_srchEntry = nullptr;
    QLabel *m_srchNote = nullptr;

    /* disassembly */
    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    uint64_t m_seenSerial = 0;          /* apple2debug_stop_serial last followed */
    int m_selected = -1;                /* the selected line's address, or -1 */
    int m_wheel = 0;                    /* wheel travel not yet a row */
    std::vector<uint16_t> m_lineAddr;

    /* video pages */
    QComboBox *m_viewPick = nullptr;
    QLabel *m_viewPic = nullptr, *m_viewNote = nullptr;
    std::vector<uint32_t> m_viewFb;
    /* the click-to-inspect: the page drawn at 2x (no marks), what is under
     * the mouse and what is pinned */
    QImage m_viewImg;
    QLabel *m_viewHoverTxt = nullptr, *m_viewPinTxt = nullptr, *m_viewMsg = nullptr;
    bool m_viewHover = false, m_viewPinned = false;
    apple2debug_pixel m_viewHoverPx = {}, m_viewPinPx = {};
    int m_viewPinX = 0, m_viewPinY = 0;
    void viewHover(int x, int y);       /* picture coords, or -1 to clear */
    void viewPin(int x, int y);
    void viewUnpin();
    void viewBreak(int x, int y);
    void viewDraw();

    /* breakpoints */
    QTableWidget *m_bpTable = nullptr;
    QString m_bpSig;                    /* what the table shows, to rebuild on change */
    bool m_bpFilling = false;
    QLabel *m_bpEmpty = nullptr, *m_bpMsg = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;

    QLabel *m_bpStop = nullptr;

    /* watches */
    QLineEdit *m_wtEntry = nullptr;
    QTableWidget *m_wtTable = nullptr;
    unsigned m_wtGen = ~0u;

    /* slots and I/O */
    QTableWidget *m_slotTable = nullptr, *m_mapTable = nullptr;
    QPlainTextEdit *m_io = nullptr;

    /* trace (0, live) and history (1, on a stop) share the engine's ring */
    QPushButton *m_hRec[2] = {};
    QComboBox *m_hFilter[2] = {};
    QLineEdit *m_hRange[2] = {};
    QPlainTextEdit *m_hText[2] = {};
    QLabel *m_hNote[2] = {};
    int m_hSeen[2] = { -1, -1 };
    bool m_hSync = false;

    /* tabs peeled off into windows of their own */
    QWidget *m_pages[16] = {};
    QWidget *m_popped[16] = {};
    bool m_closing = false;

    /* fujinet */
    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
};
