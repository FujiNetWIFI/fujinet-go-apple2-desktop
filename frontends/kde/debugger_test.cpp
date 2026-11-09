/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running Apple II
 * session (no FujiNet, no audio) whose RAM holds a hand-assembled loop
 * written through the debugger (needs the Apple II ROMs; skips without
 * them): the tabs, attach/detach with show and hide, the stepping keys and
 * buttons, the console (transcript, Tab completion, history), the
 * disassembly (the PC's bar, click / double-click / F9 for a breakpoint,
 * selection and Run to Cursor, the wheel, the jump box), the register, flag
 * and memory entries, the Breakpoints, Watches, Slots & I/O, Trace, History,
 * Video and FujiNet tabs, and Pop Out / Dock.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1: in the
 * light scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "apple2debug.h"
#include "apple2memtools.h"
#include "apple2session.h"
#include "apple2vidtab.h"
#include "debugger/DebuggerWindow.h"

namespace {

/* The test subject, in main RAM at $0300:
 *   0300  LDX #3
 *   0302  JSR $0320      ; INY / RTS
 *   0305  DEX
 *   0306  BNE $0302
 *   0308  INC $0900      ; write $0900
 *   030B  LDA $0900      ; read $0900
 *   030E  JMP $0300 */
const uint8_t kProg[] = {
    0xA2, 0x03, 0x20, 0x20, 0x03, 0xCA, 0xD0, 0xFA, 0xEE, 0x00, 0x09, 0xAD, 0x00, 0x09, 0x4C, 0x00, 0x03,
};
const uint8_t kSub[] = { 0xC8, 0x60 };

/* With the machine stopped: the program in place, PC at its first line. */
void loadProgram(apple2debug *d)
{
    for (size_t i = 0; i < sizeof kProg; i++) apple2debug_write(d, (uint16_t)(0x0300 + i), kProg[i]);
    for (size_t i = 0; i < sizeof kSub; i++) apple2debug_write(d, (uint16_t)(0x0320 + i), kSub[i]);
    apple2debug_cpu_set(d, APPLE2_FLAG_I, 1);
    apple2debug_cpu_set(d, APPLE2_FLAG_D, 0);
    apple2debug_cpu_set(d, APPLE2_REG_SP, 0x01FF);
    apple2debug_cpu_set(d, APPLE2_REG_PC, 0x0300);
}

int pcOf(apple2debug *d)
{
    apple2debug_cpu c;
    apple2debug_cpu_get(d, &c);
    return c.pc;
}

} // namespace

/* A pointer move on the video picture, sent straight to the widget. Before
 * Qt 6.8 QTest::mouseMove did not always deliver the move to a widget the
 * pointer had not been "over", so hover assertions were intermittent; the
 * window's event filter sees this exactly as it sees a real move. */
static void movePicture(QWidget *pic, const QPoint &at)
{
    QMouseEvent ev(QEvent::MouseMove, QPointF(at), QPointF(pic->mapToGlobal(at)), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(pic, &ev);
    QCoreApplication::processEvents();
}

/* A click on the video picture that cannot be read as the second half of a
 * double-click. Qt Test stamps its events with synthetic times, and before Qt
 * 6.8 two clicks on the same spot a few synthetic milliseconds apart (with
 * anything in between) became a double-click, which pins and adds a
 * breakpoint instead of unpinning. Waiting out the interval makes the clicks
 * mean the same on every Qt. */
static void clickPicture(QWidget *pic, const QPoint &at)
{
    QTest::mouseClick(pic, Qt::LeftButton, {}, at,
                      QGuiApplication::styleHints()->mouseDoubleClickInterval() + 25);
}

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    apple2session *m_s = nullptr;
    apple2debug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v;
        return nullptr;
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) || l->text() == QLatin1String("Running")) return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }
    QPlainTextEdit *memView() { return m_tabs->widget(2)->findChild<QPlainTextEdit *>(); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_is_stopped(m_d), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() && statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(2, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        apple2session_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";                 /* no FujiNet runtime */
        m_s = apple2session_new(&paths);
        QVERIFY(m_s);
        /* No FujiNet, audio or gamepad: this is about the window, and it
         * keeps the test off the network and off the sound device. */
        apple2session_start_opts opts;
        apple2session_default_opts(m_s, &opts);
        opts.enable_fujinet = 0;
        opts.enable_audio = 0;
        opts.enable_gamepad = 0;
        opts.slot7 = "Empty";
        if (apple2session_start(m_s, &opts) != 0)
            QSKIP(qPrintable(QStringLiteral("no Apple II ROMs (%1)").arg(QString::fromUtf8(apple2session_last_error(m_s)))));
        m_d = apple2session_debugger(m_s);
        QVERIFY(m_d);

        /* the window opens on the tab APPLE2_DEBUGGER_TAB names */
        qputenv("APPLE2_DEBUGGER_TAB", "i/o");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("APPLE2_DEBUGGER_TAB");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 5);
        m_tabs->setCurrentIndex(0);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        if (m_s) {
            apple2session_stop(m_s);
            apple2session_free(m_s);
        }
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_is_attached(m_d), 2000);
        if (!apple2debug_is_stopped(m_d)) apple2debug_stop(m_d);
        waitStopped();
        apple2debug_breakpoint_clear(m_d);
        loadProgram(m_d);
    }

    void tabsAndAttach()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("Disassembly"),
                                      QStringLiteral("Memory"), QStringLiteral("Breakpoints"),
                                      QStringLiteral("Watches"), QStringLiteral("Slots && I/O"),
                                      QStringLiteral("Trace"), QStringLiteral("History"),
                                      QStringLiteral("Video"), QStringLiteral("FujiNet") }));
        QVERIFY(apple2debug_is_attached(m_d));
        QVERIFY(apple2debug_is_stopped(m_d));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!apple2debug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
    }

    /* Hiding detaches (the machine runs on), showing attaches (and stops it). */
    void hideDetachesShowAttaches()
    {
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!apple2debug_is_attached(m_d), 2000);
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_is_attached(m_d), 2000);
        waitStopped();
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), 0x0300);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x0302, 2000);
        QCOMPARE(statusLabel()->text().left(13), QStringLiteral("Stopped: step"));
        /* F8 steps over the JSR */
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x0305, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step over")));
        /* the buttons do the same: Out from inside the subroutine */
        apple2debug_write(m_d, 0x01FE, 0x04);           /* the return address, as a JSR left it */
        apple2debug_write(m_d, 0x01FF, 0x03);
        apple2debug_cpu_set(m_d, APPLE2_REG_SP, 0x01FD);
        apple2debug_cpu_set(m_d, APPLE2_REG_PC, 0x0320);
        QTest::mouseClick(button(QStringLiteral("Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x0305, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step out")));
        /* Scanline and Frame (Shift+F7) land on the beam's next line / frame */
        apple2debug_cpu c, c2;
        apple2debug_cpu_get(m_d, &c);
        QTest::mouseClick(button(QStringLiteral("Scanline")), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_is_stopped(m_d) &&
                                     (apple2debug_cpu_get(m_d, &c2), c2.line != c.line || c2.frame != c.frame), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("scanline")), 2000);
        apple2debug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_is_stopped(m_d) &&
                                     (apple2debug_cpu_get(m_d, &c2), c2.frame > c.frame), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("frame")), 2000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!apple2debug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("set pc $0308"));
        QCOMPARE(pcOf(m_d), 0x0308);
        typeInto(in, QStringLiteral("set pc $0300"));
        typeInto(in, QStringLiteral("break 770"));      /* decimal, the engine's rule: $0302 */
        QVERIFY(apple2debug_breakpoint_check(m_d, 0x0302));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint: exec $0302")), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), 0x0302);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint: exec $0302")));
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("reg"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("regs "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("b"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("break")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(in->text().startsWith(QLatin1Char('b')));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("run"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("break 770"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("run"));
        in->clear();
        shot(QStringLiteral("console"));
    }

    /* A symbol file loaded through the console shows in the status line. */
    void symbols()
    {
        m_tabs->setCurrentIndex(0);
        QFile sym(m_dir.path() + QStringLiteral("/loop.sym"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("0305 afterwards\n");
        sym.close();
        QLineEdit *in = entry(QStringLiteral("command"));
        typeInto(in, QStringLiteral("labels ") + sym.fileName());
        QCOMPARE(apple2debug_label_address(m_d, "afterwards"), 0x0305);
        typeInto(in, QStringLiteral("break afterwards"));
        QVERIFY(apple2debug_breakpoint_check(m_d, 0x0305));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_is_stopped(m_d) && pcOf(m_d) == 0x0305, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("(afterwards)")), 2000);
        apple2debug_set_label(m_d, 0x0305, nullptr);
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0300) >= 0, 2000);
        const int pcLine = lineOf(v, 0x0300);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY(pcLine >= rows / 3 - 2 && pcLine <= rows / 3 + 2);
        /* the PC's bar is the block's own background (an extra selection
         * would paint over the breakpoint dot) */
        QCOMPARE(v->document()->findBlockByNumber(pcLine).blockFormat().background().color(),
                 QColor(0xf4, 0x43, 0x36));
        for (const auto &s : v->extraSelections())
            QVERIFY(!s.format.boolProperty(QTextFormat::FullWidthSelection));

        /* a breakpoint's dot: filled and red, hollow and grey when disabled,
         * and white on the PC's own line (its bar is nearly the same red) */
        apple2debug_breakpoint_toggle(m_d, 0x0302);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x0302)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, 0x0302)), QColor(0xe0, 0x1b, 0x24));
        apple2debug_breakpoint_toggle(m_d, 0x0300);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x0300)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0x0300)), QColor(Qt::white));
        apple2debug_breakpoint_toggle(m_d, 0x0300);
        apple2debug_breakpoint b;
        QCOMPARE(apple2debug_breakpoint_list(m_d, &b, 1), 1);
        apple2debug_breakpoint_enable(m_d, b.id, 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x0302)).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0x0302)), QColor(0x9a, 0x99, 0x96));
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0305) >= 0, 2000);
        /* nothing selected: Run to Cursor and F9 say so */
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Select a line")));
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(0x0305, 30);
        QVERIFY(!apple2debug_breakpoint_check(m_d, 0x0305));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, 0x0305) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(0x0308, 0);
        QVERIFY(apple2debug_breakpoint_check(m_d, 0x0308));
        clickLine(0x0308, 0);
        QVERIFY(!apple2debug_breakpoint_check(m_d, 0x0308));
        clickLine(0x030B, 30, true);
        QVERIFY(apple2debug_breakpoint_check(m_d, 0x030B));
        apple2debug_breakpoint_toggle(m_d, 0x030B);
        clickLine(0x030E, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(apple2debug_breakpoint_check(m_d, 0x030E));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!apple2debug_breakpoint_check(m_d, 0x030E));
        apple2debug_breakpoint b[4];
        QCOMPARE(apple2debug_breakpoint_list(m_d, b, 4), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0308) >= 0, 2000);
        clickLine(0x0308, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x0308, 3000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("run to")));
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0300) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entry(QStringLiteral("$address"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("0320"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0320) >= 0, 2000);
        apple2debug_set_label(m_d, 0x0305, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0305) >= 0, 2000);
        QVERIFY(v->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$0320+1"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0321) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0300) >= 0, 2000);
        apple2debug_set_label(m_d, 0x0305, nullptr);
    }

    void registersFlagsMemory()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().startsWith(QLatin1String("Enter sets it"))) regs << e;
        QCOMPARE(regs.size(), 6);                   /* PC SP A X Y P */
        QTRY_COMPARE_WITH_TIMEOUT(regs[0]->text(), QStringLiteral("0300"), 2000);   /* PC */
        QCOMPARE(regs[1]->text(), QStringLiteral("01FF"));                           /* SP */
        typeInto(regs[0], QStringLiteral("0308"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x0308, 2000);
        typeInto(regs[2], QStringLiteral("5A"));    /* A */
        apple2debug_cpu c;
        apple2debug_cpu_get(m_d, &c);
        QCOMPARE(c.a, 0x5A);
        /* the flags */
        QCheckBox *carry = check(QStringLiteral("C"));
        QVERIFY(carry);
        apple2debug_cpu_set(m_d, APPLE2_FLAG_C, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        apple2debug_cpu_get(m_d, &c);
        QCOMPARE(c.cf, 1);
        QCheckBox *unused = check(QStringLiteral("-"));
        QVERIFY(unused && !unused->isEnabled());
        apple2debug_cpu_set(m_d, APPLE2_FLAG_C, 0);
        /* the beam / cycles line */
        bool beam = false;
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("line"))) beam = l->text().contains(QLatin1String("cycles"));
        QVERIFY(beam);
        /* memory: write through the map, go to a label or an address */
        QLineEdit *addr = entryIs(QStringLiteral("$C000"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("0910"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        uint8_t b = 0;
        apple2debug_read(m_d, 0x0910, &b, 1);
        QCOMPARE((int)b, 0x5A);
        addr->setText(QStringLiteral("$0911"));
        typeInto(val, QStringLiteral("$A5"));
        apple2debug_read(m_d, 0x0911, &b, 1);
        QCOMPARE((int)b, 0xA5);
        apple2debug_write(m_d, 0x0912, 0x11);
        addr->setText(QStringLiteral("0912"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        apple2debug_read(m_d, 0x0912, &b, 1);
        QCOMPARE((int)b, 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        QLineEdit *go = entryIs(QStringLiteral("$C000, label or lo-hi"));
        QVERIFY(go);
        typeInto(go, QStringLiteral("0300"));
        QPlainTextEdit *mem = memView();
        QVERIFY(mem);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0300: A2 03 20 20 03 CA D0 FA")));
        /* the bytes Go to found are marked in the accent colour, white ink */
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == QColor(0xf4, 0x43, 0x36) &&
                      s.format.foreground().color() == QColor(Qt::white);
        QVERIFY(marked);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!apple2debug_is_stopped(m_d), 2000);
        typeInto(regs[0], QStringLiteral("0300"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("memory"));
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("start"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        /* execute, read and write: no port types on a 6502 */
        QStringList types;
        for (int i = 0; i < type->count(); ++i) types << type->itemText(i);
        QCOMPARE(types, (QStringList{ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                                      QStringLiteral("Read/Write") }));
        QPushButton *add = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Add")) add = b;
        QPushButton *clear = button(QStringLiteral("Clear All"));
        QVERIFY(button(QStringLiteral("Clear Hits")));
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && msg);

        /* exec at a hex address */
        typeInto(start, QStringLiteral("0302"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 1)->text().contains(QStringLiteral("$0302")));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write watchpoint over a range, with a condition */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("0900"));
        end->setText(QStringLiteral("$09FF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QVERIFY(t->item(1, 1)->text().contains(QStringLiteral("$0900-$09FF")));
        QVERIFY(t->item(1, 1)->text().contains(QStringLiteral("value > 0")));
        /* errors are shown, not swallowed */
        cond->setText(QStringLiteral("nosuchname == 1"));
        start->setText(QStringLiteral("7000"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), 2);
        cond->clear();
        start->setText(QStringLiteral("10000"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), 2);
        start->setText(QStringLiteral("FF"));
        end->setText(QStringLiteral("FC"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("Execute"));

        /* the exec breakpoint stops the run, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_is_stopped(m_d) && pcOf(m_d) == 0x0302, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $0302")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 2) && t->item(0, 2)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write watch then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        apple2debug_breakpoint list[2];
        QCOMPARE(apple2debug_breakpoint_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("write $0900")),
                                 4000);
        QVERIFY(pcOf(m_d) != 0x0302);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 3));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(apple2debug_breakpoint_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(apple2debug_breakpoint_list(m_d, list, 2), 0);
    }

    /* Watches: add, live value, Memory, Break on change, Remove. */
    void watchesTab()
    {
        apple2watch_clear();
        apple2debug_write(m_d, 0x0910, 0x42);
        m_tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTableWidget *t = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *e = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(t && e);
        QCOMPARE(t->rowCount(), 0);
        typeInto(e, QStringLiteral("$0910"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$0910")));
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$42")));
        apple2debug_write(m_d, 0x0910, 0x43);
        QTest::keyClick(m_win, Qt::Key_F7);             /* a stop refreshes the values */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        typeInto(e, QStringLiteral("$0920-$092F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        /* while the machine runs the value is a dash */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!apple2debug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QLatin1String("-")) &&
                                     !t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("watches"));

        /* Break on change adds a write breakpoint with the condition */
        apple2debug_breakpoint_clear(m_d);
        auto buttonOf = [&](int row, const QString &text) -> QPushButton * {
            for (QPushButton *b : t->cellWidget(row, 1)->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QPushButton *brk = buttonOf(0, QStringLiteral("Break on change"));
        QVERIFY(brk);
        QTest::mouseClick(brk, Qt::LeftButton);
        apple2debug_breakpoint bp[2];
        QCOMPARE(apple2debug_breakpoint_list(m_d, bp, 2), 1);
        QVERIFY(bp[0].type & APPLE2DEBUG_BP_WRITE);
        QCOMPARE((int)bp[0].start, 0x0910);
        QVERIFY(QString::fromUtf8(bp[0].condition).contains(QStringLiteral("old")));
        apple2debug_breakpoint_clear(m_d);

        /* Memory switches to the Memory tab with the range marked */
        QPushButton *mem = buttonOf(1, QStringLiteral("Memory"));
        QVERIFY(mem);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0920: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());

        m_tabs->setCurrentIndex(4);
        QTest::mouseClick(buttonOf(1, QStringLiteral("Remove")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(apple2watch_count(), 1);
        apple2watch_clear();
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Go to marks a range; Find Next / Previous step through hits. */
    void memorySearch()
    {
        const uint8_t pat[6] = { 0xDE, 0xEE, 0xDE, 0xEE, 0x5A, 0xA5 };
        for (int i = 0; i < 6; ++i) apple2debug_write(m_d, (uint16_t)(0x0A00 + i), pat[i]);
        for (int i = 0; i < 6; ++i) apple2debug_write(m_d, (uint16_t)(0x0B00 + i), pat[i]);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *go = entryIs(QStringLiteral("$C000, label or lo-hi"));
        typeInto(go, QStringLiteral("0A00-0A05"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0A00: DE EE DE EE 5A A5")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* a label also works */
        apple2debug_set_label(m_d, 0x0B00, "found");
        typeInto(go, QStringLiteral("found"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0B00: DE EE")));
        apple2debug_set_label(m_d, 0x0B00, nullptr);

        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("gray")) && l->text().isEmpty()) note = l;
        QVERIFY(note);
        typeInto(go, QStringLiteral("0A00-0A05"));
        find->setText(QStringLiteral("DE EE DE EE 5A"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $0B00"));          /* after the marked one */
        QTest::mouseClick(button(QStringLiteral("Find Previous")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $0A00"));
        find->setText(QStringLiteral("DE EE DE EE 5A A5 FF 77 66"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Not found"));
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));
        /* it needs a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!apple2debug_is_stopped(m_d), 2000);
        find->setText(QStringLiteral("DE EE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Stop the machine first"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
    }

    /* Trace and History share the ring and the recorder. */
    void traceAndHistory()
    {
        apple2debug_trace_enable(m_d, 0);
        apple2debug_trace_clear(m_d);
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *rec = m_tabs->currentWidget()->findChild<QPushButton *>();
        QVERIFY(rec && rec->text() == QLatin1String("Record"));
        QPushButton *rec2 = m_tabs->widget(7)->findChild<QPushButton *>();
        QVERIFY(rec2 && rec2 != rec);
        QVERIFY(!rec->isChecked());
        QTest::mouseClick(rec, Qt::LeftButton);
        QVERIFY(apple2debug_trace_enabled(m_d));
        QVERIFY(rec->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(rec2->isChecked(), 2000);                   /* in sync */
        QTest::keyClick(m_win, Qt::Key_F5);                                  /* runs */
        QTRY_VERIFY_WITH_TIMEOUT(!apple2debug_is_stopped(m_d), 2000);
        auto *tr = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(tr->toPlainText().contains(QStringLiteral("INC")), 3000);   /* live */
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(apple2debug_trace_count(m_d) > 20, 2000);
        /* History shows the same ring, oldest first, at the stop */
        m_tabs->setCurrentIndex(7);
        auto *hist = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("INC")), 2000);
        QVERIFY(hist->toPlainText().count(QLatin1Char('\n')) < 2000);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().contains(QLatin1String("instructions"))) note = l;
        QVERIFY(note && note->text().startsWith(QLatin1String("recording")));
        shot(QStringLiteral("history"));
        /* a branches-only filter leaves jumps, calls and returns */
        auto *filter = m_tabs->currentWidget()->findChild<QComboBox *>();
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC $0900")), 2000);
        QVERIFY(hist->toPlainText().contains(QStringLiteral("JMP ")));
        /* a range filter */
        filter->setCurrentIndex(2);
        auto *range = m_tabs->currentWidget()->findChild<QLineEdit *>();
        typeInto(range, QStringLiteral("0308-0308"));
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("INC $0900")) &&
                                     !hist->toPlainText().contains(QStringLiteral("DEX")), 2000);
        filter->setCurrentIndex(0);
        /* Clear */
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear")) QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(apple2debug_trace_count(m_d), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC $0900")), 2000);
        /* and off, from either tab */
        QTest::mouseClick(m_tabs->widget(7)->findChild<QPushButton *>(), Qt::LeftButton);
        QVERIFY(!apple2debug_trace_enabled(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(6)->findChild<QPushButton *>()->isChecked(), 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Any tab can leave for a window of its own and come back to its place. */
    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(3);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 9);
        QWidget *win = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Apple II Debugger - Breakpoints")) win = w;
        QVERIFY(win);
        QTRY_VERIFY_WITH_TIMEOUT(win->isVisible(), 1000);
        QVERIFY(win->findChild<QTableWidget *>());
        QCOMPARE(win->property("apple2HelpContext").toString(), QStringLiteral("debugger/breakpoints"));
        /* the popped tab keeps refreshing: a breakpoint added shows in its window */
        apple2debug_breakpoint_toggle(m_d, 0x0302);
        QTRY_COMPARE_WITH_TIMEOUT(win->findChild<QTableWidget *>()->rowCount(), 1, 2000);
        apple2debug_breakpoint_clear(m_d);
        shot(QStringLiteral("popout"));
        QPushButton *dockBtn = nullptr;
        for (QPushButton *b : win->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) dockBtn = b;
        QVERIFY(dockBtn);
        QTest::mouseClick(dockBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 10, 1000);
        QCOMPARE(m_tabs->tabText(3), QStringLiteral("Breakpoints"));       /* back in its place */
        /* closing the window docks it too; several can be out, but one stays */
        m_tabs->setCurrentIndex(5);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 9);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Trace"));
        QWidget *slotsWin = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("Apple II Debugger - Slots & I/O")) slotsWin = w;
        QVERIFY(slotsWin);
        slotsWin->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 10, 1000);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Slots && I/O"));
        for (int i = 0; i < 9; ++i) {
            m_tabs->setCurrentIndex(0);
            QTest::mouseClick(pop, Qt::LeftButton);
        }
        QCOMPARE(m_tabs->count(), 1);
        QTest::mouseClick(pop, Qt::LeftButton);                          /* the last stays */
        QCOMPARE(m_tabs->count(), 1);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle().startsWith(QLatin1String("Apple II Debugger - "))) w->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 10, 1000);
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names.first(), QStringLiteral("Console"));
        QCOMPARE(names.last(), QStringLiteral("FujiNet"));
        QCOMPARE(names.at(5), QStringLiteral("Slots && I/O"));
    }

    /* A new stop pulls the disassembly back to the PC from wherever it was
     * scrolled, and the Breakpoints tab says why it stopped. */
    void stopRecentresAndNames()
    {
        m_tabs->setCurrentIndex(1);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x0300) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -600), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_VERIFY_WITH_TIMEOUT(follow->isChecked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, pcOf(m_d)) >= 0, 2000);
        m_tabs->setCurrentIndex(3);
        QLabel *stop = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped"))) stop = l;
        QVERIFY(stop);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stop->text() == QLatin1String("Running."), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
    }

    /* Slots & I/O: the slot table, the memory map and the soft switches. */
    void slotsTab()
    {
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        const QList<QTableWidget *> tables = m_tabs->currentWidget()->findChildren<QTableWidget *>();
        QCOMPARE(tables.size(), 2);
        QTableWidget *slotTable = tables[0], *mapTable = tables[1];
        QTRY_VERIFY_WITH_TIMEOUT(slotTable->rowCount() >= 8, 2000);
        QCOMPARE(slotTable->columnCount(), 3);
        QTRY_VERIFY_WITH_TIMEOUT(mapTable->rowCount() >= 1, 2000);
        QVERIFY(mapTable->item(0, 0)->text().startsWith(QStringLiteral("$0000-")));
        QVERIFY(mapTable->item(mapTable->rowCount() - 1, 0)->text().endsWith(QStringLiteral("$FFFF")));
        auto *io = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QVERIFY(io);
        QTRY_VERIFY_WITH_TIMEOUT(!io->toPlainText().isEmpty(), 2000);
        shot(QStringLiteral("slots"));
    }

private:
    /* ---- the Video tab ---- */

    QWidget *videoPage() { return m_tabs->widget(8); }
    template <class T> T *vw(const char *name) { return videoPage()->findChild<T *>(QString::fromLatin1(name)); }
    void showVideo()
    {
        m_tabs->setCurrentIndex(8);
        QTRY_VERIFY_WITH_TIMEOUT(videoPage()->isVisible(), 1000);
    }
    static void pickText(QComboBox *c, const QString &text)
    {
        const int i = c->findText(text);
        QVERIFY2(i >= 0, qPrintable(text));
        c->setCurrentIndex(i);
    }
    /* Decoded, Text 40, page 1, zoom 2x, Follow off: byte cell (col, row) is the pixel at 2 * (14 col, 8 row) */
    void decodedText40(QComboBox *zoomPick = nullptr)
    {
        showVideo();
        pickText(vw<QComboBox>("videoView"), QStringLiteral("Decoded"));
        pickText(vw<QComboBox>("videoMode"), QStringLiteral("Text 40"));
        pickText(vw<QComboBox>("videoPage"), QStringLiteral("Page 1"));
        QCheckBox *mixed = vw<QCheckBox>("videoMixed");
        if (mixed->isChecked()) mixed->setChecked(false);
        pickText(zoomPick ? zoomPick : vw<QComboBox>("videoZoom"), QStringLiteral("2×"));
    }
    static bool hasColour(const QImage &img, int x0, int x1, int y, int r, int g, int b)
    {
        for (int x = x0; x <= x1; ++x) {
            const QColor c = img.pixelColor(x, y);
            if (qAbs(c.red() - r) < 24 && qAbs(c.green() - g) < 24 && qAbs(c.blue() - b) < 24) return true;
        }
        return false;
    }

private slots:
    /* Views, Follow screen, mode / page / Mixed, zoom, overlays. */
    void videoTab()
    {
        apple2debug_breakpoint_clear(m_d);
        showVideo();
        auto *view = vw<QComboBox>("videoView");
        auto *zoom = vw<QComboBox>("videoZoom");
        auto *mode = vw<QComboBox>("videoMode");
        auto *page = vw<QComboBox>("videoPage");
        auto *follow = vw<QCheckBox>("videoFollow");
        auto *mixed = vw<QCheckBox>("videoMixed");
        QWidget *pic = vw<QWidget>("videoPicture");
        QVERIFY(view && zoom && mode && page && follow && mixed && pic);
        QStringList views, modes, zooms;
        for (int i = 0; i < view->count(); ++i) views << view->itemText(i);
        for (int i = 0; i < mode->count(); ++i) modes << mode->itemText(i);
        for (int i = 0; i < zoom->count(); ++i) zooms << zoom->itemText(i);
        QCOMPARE(views, (QStringList{ QStringLiteral("Screen"), QStringLiteral("Decoded") }));
        QCOMPARE(modes, (QStringList{ QStringLiteral("Text 40"), QStringLiteral("Text 80"), QStringLiteral("Lo-res"),
                                      QStringLiteral("Double lo-res"), QStringLiteral("Hi-res"),
                                      QStringLiteral("Double hi-res") }));
        QCOMPARE(zooms, (QStringList{ QStringLiteral("Fit"), QStringLiteral("1×"), QStringLiteral("2×"),
                                      QStringLiteral("3×"), QStringLiteral("4×") }));
        QCOMPARE(page->count(), 2);
        shot(QStringLiteral("video-screen"));

        /* Decoded is 560x192 before zoom */
        pickText(view, QStringLiteral("Decoded"));
        QVERIFY(follow->isChecked());           /* Follow screen is on by default */
        pickText(zoom, QStringLiteral("1×"));
        QTRY_COMPARE_WITH_TIMEOUT(pic->size(), QSize(APPLE2VID_W, APPLE2VID_H), 1000);
        pickText(zoom, QStringLiteral("2×"));
        QTRY_COMPARE_WITH_TIMEOUT(pic->size(), QSize(APPLE2VID_W * 2, APPLE2VID_H * 2), 1000);
        pickText(zoom, QStringLiteral("4×"));
        QTRY_COMPARE_WITH_TIMEOUT(pic->size(), QSize(APPLE2VID_W * 4, APPLE2VID_H * 4), 1000);
        pickText(zoom, QStringLiteral("Fit"));
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100 && pic->height() >= 100, 1000);
        pickText(zoom, QStringLiteral("1×"));
        QTRY_COMPARE_WITH_TIMEOUT(pic->size(), QSize(APPLE2VID_W, APPLE2VID_H), 1000);
        shot(QStringLiteral("video-decoded"));

        /* the controls track the machine while Follow is on */
        apple2vid_tab *ref = apple2vid_tab_new(m_s);
        apple2vid_tab_refresh(ref, 1);
        QCOMPARE(mode->currentIndex(), apple2vid_tab_mode(ref));
        QCOMPARE(page->currentIndex() + 1, apple2vid_tab_page(ref));
        QCOMPARE((int)mixed->isChecked(), apple2vid_tab_mixed(ref) ? 1 : 0);
        apple2vid_tab_free(ref);

        /* choosing a mode, a page or Mixed turns Follow off */
        pickText(mode, QStringLiteral("Hi-res"));
        QVERIFY(!follow->isChecked());
        QCOMPARE(mode->currentText(), QStringLiteral("Hi-res"));
        follow->setChecked(true);
        pickText(page, QStringLiteral("Page 2"));
        QVERIFY(!follow->isChecked());
        QCOMPARE(page->currentText(), QStringLiteral("Page 2"));
        follow->setChecked(true);
        mixed->setChecked(!mixed->isChecked());
        QVERIFY(!follow->isChecked());
        const bool wasMixed = !mixed->isChecked();
        /* a Mixed hi-res picture differs from the plain one in its text lines */
        pickText(mode, QStringLiteral("Hi-res"));
        mixed->setChecked(false);
        const QImage plain = pic->grab().toImage();
        mixed->setChecked(true);
        const QImage withText = pic->grab().toImage();
        QVERIFY(plain != withText);
        mixed->setChecked(wasMixed);
        /* Follow screen again returns to the machine's mode */
        follow->setChecked(true);
        ref = apple2vid_tab_new(m_s);
        apple2vid_tab_refresh(ref, 1);
        QTRY_COMPARE_WITH_TIMEOUT(mode->currentIndex(), apple2vid_tab_mode(ref), 1000);
        apple2vid_tab_free(ref);

        /* an overlay checkbox redraws the picture */
        pickText(mode, QStringLiteral("Text 40"));
        pickText(page, QStringLiteral("Page 1"));
        mixed->setChecked(false);
        auto *grid = vw<QCheckBox>("videoOverlay_grid");
        QVERIFY(grid && !grid->isChecked());
        const QImage before = pic->grab().toImage();
        grid->setChecked(true);
        const QImage after = pic->grab().toImage();
        QVERIFY(before != after);
        shot(QStringLiteral("video-grid"));
        grid->setChecked(false);
        QCOMPARE(pic->grab().toImage(), before);
        /* one checkbox per overlay in the table */
        int n = 0;
        const apple2vid_overlay_info *od = apple2vid_overlays(&n);
        for (int i = 0; i < n; ++i)
            QVERIFY(vw<QCheckBox>(qPrintable(QStringLiteral("videoOverlay_%1").arg(QString::fromUtf8(od[i].name)))));

        pickText(view, QStringLiteral("Screen"));
        QCOMPARE(mode->isEnabled(), false);             /* the Decoded controls only apply to Decoded */
        pickText(view, QStringLiteral("Decoded"));
        QCOMPARE(mode->isEnabled(), true);
        pickText(zoom, QStringLiteral("1×"));
        m_tabs->setCurrentIndex(0);
    }

    /* Hover reads a byte, a click pins it, the same spot unpins, a double-click breaks on writes to it,
     * and Show in Memory opens the Memory tab on it. Text page 1 cell (3,2) = $0503. */
    void videoInspector()
    {
        apple2debug_breakpoint_clear(m_d);
        apple2debug_write(m_d, 0x0503, 0xC1);
        decodedText40();
        QWidget *pic = vw<QWidget>("videoPicture");
        auto *pixel = vw<QLabel>("videoPixel");
        auto *note = vw<QLabel>("videoNote");
        QVERIFY(pic && pixel && note);
        auto *sections = vw<QTabWidget>("videoSections");
        QCOMPARE(sections->tabText(0), QStringLiteral("Pixel"));
        sections->setCurrentIndex(0);
        QVERIFY(pixel->text().contains(QLatin1String("Move over")));
        const QPoint at(3 * 14 * 2 + 12, 2 * 8 * 2 + 6);        /* inside the cell, at 2x */
        const QPoint away(30 * 14 * 2 + 12, 10 * 8 * 2 + 6);
        movePicture(pic, at);
        QTRY_VERIFY_WITH_TIMEOUT(pixel->text().contains(QLatin1String("0503")), 2000);
        clickPicture(pic, at);
        movePicture(pic, away);                    /* the pin stays when the pointer leaves */
        QVERIFY(pixel->text().contains(QLatin1String("0503")));
        /* marks: the pin is yellow around the cell (2x: x = 84, y = 32 .. 48) */
        QImage img = pic->grab().toImage();
        QVERIFY(hasColour(img, 82, 86, 40, 255, 217, 26));
        shot(QStringLiteral("video-pixel"));
        /* the hover mark follows the pointer: white around the new cell (x = 840) */
        QTRY_VERIFY_WITH_TIMEOUT(hasColour(pic->grab().toImage(), 838, 842, 100, 255, 255, 255), 1000);
        clickPicture(pic, at);     /* the same spot unpins */
        movePicture(pic, away);
        QTRY_VERIFY_WITH_TIMEOUT(!pixel->text().contains(QLatin1String("0503")), 1000);
        QVERIFY(!hasColour(pic->grab().toImage(), 82, 86, 40, 255, 217, 26));
        clickPicture(pic, at);
        QTRY_VERIFY_WITH_TIMEOUT(pixel->text().contains(QLatin1String("0503")), 1000);

        /* a double-click adds a write breakpoint on the byte (Text 40 uses no aux), once */
        apple2debug_breakpoint_clear(m_d);
        QTest::mouseDClick(pic, Qt::LeftButton, {}, at);
        QTRY_VERIFY_WITH_TIMEOUT(note->text().contains(QLatin1String("0503")), 1000);
        apple2debug_breakpoint bps[16];
        int n = apple2debug_breakpoint_list(m_d, bps, 16);
        int writes = 0;
        for (int i = 0; i < n; i++)
            if ((bps[i].type & APPLE2DEBUG_BP_WRITE) && bps[i].start == 0x0503 && bps[i].end == 0x0503) writes++;
        QCOMPARE(n, 1);
        QCOMPARE(writes, 1);
        QTest::mouseDClick(pic, Qt::LeftButton, {}, at);
        QCOMPARE(apple2debug_breakpoint_list(m_d, bps, 16), 1);
        /* the button does the same on the pinned pixel */
        apple2debug_breakpoint_clear(m_d);
        QPushButton *brk = nullptr, *mem = nullptr, *unpin = nullptr;
        for (QPushButton *b : videoPage()->findChildren<QPushButton *>()) {
            if (b->text() == QLatin1String("Break on writes")) brk = b;
            if (b->text() == QLatin1String("Show in Memory")) mem = b;
            if (b->text() == QLatin1String("Unpin")) unpin = b;
        }
        QVERIFY(brk && mem && unpin);
        QTest::mouseClick(brk, Qt::LeftButton);
        QCOMPARE(apple2debug_breakpoint_list(m_d, bps, 16), 1);

        /* Text 80 uses aux too: two write breakpoints (main and aux) */
        apple2debug_breakpoint_clear(m_d);
        pickText(vw<QComboBox>("videoMode"), QStringLiteral("Text 80"));
        QTest::mouseDClick(pic, Qt::LeftButton, {}, QPoint(3 * 14 * 2 + 4, 2 * 8 * 2 + 6));
        n = apple2debug_breakpoint_list(m_d, bps, 16);
        QCOMPARE(n, apple2debug_bank_valid(m_d, APPLE2_BANK_AUX) ? 2 : 1);
        for (int i = 0; i < n; i++) QVERIFY(bps[i].type & APPLE2DEBUG_BP_WRITE);
        apple2debug_breakpoint_clear(m_d);
        pickText(vw<QComboBox>("videoMode"), QStringLiteral("Text 40"));

        /* Show in Memory: the Memory tab, on the byte, in main RAM */
        QTest::mouseClick(unpin, Qt::LeftButton);
        movePicture(pic, away);
        clickPicture(pic, at);                 /* pin $0503 again */
        QTRY_VERIFY_WITH_TIMEOUT(pixel->text().contains(QLatin1String("0503")), 1000);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        auto *bank = m_tabs->widget(2)->findChild<QComboBox *>(QStringLiteral("memBank"));
        QVERIFY(bank);
        QCOMPARE(bank->currentIndex(), (int)APPLE2_BANK_MAIN);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0500: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        bank->setCurrentIndex(APPLE2_BANK_CURRENT);

        showVideo();
        QTest::mouseClick(unpin, Qt::LeftButton);
        movePicture(pic, away - QPoint(28, 0));
        movePicture(pic, away);
        /* the pin is gone (the label may still show the hovered byte, which
         * is the same one if the pointer's move was not delivered) */
        QTRY_VERIFY_WITH_TIMEOUT(!hasColour(pic->grab().toImage(), 82, 86, 40, 255, 217, 26), 1000);
        apple2debug_breakpoint_clear(m_d);
        pickText(vw<QComboBox>("videoZoom"), QStringLiteral("1×"));
        m_tabs->setCurrentIndex(0);
    }

    /* The Switches table: the eight switches, their addresses, Break on change. */
    void videoSwitches()
    {
        apple2debug_breakpoint_clear(m_d);
        decodedText40();
        auto *sections = vw<QTabWidget>("videoSections");
        QCOMPARE(sections->tabText(1), QStringLiteral("Switches"));
        sections->setCurrentIndex(1);
        auto *table = vw<QTableWidget>("videoSwitches");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), (int)APPLE2VID_SW_COUNT);
        QTRY_VERIFY_WITH_TIMEOUT(table->item(0, 0) && !table->item(0, 0)->text().isEmpty(), 2000);
        apple2vid_tab *ref = apple2vid_tab_new(m_s);
        apple2vid_tab_refresh(ref, 1);
        const apple2vid_switch *sw = nullptr;
        QCOMPARE(apple2vid_tab_switches(ref, &sw), (int)APPLE2VID_SW_COUNT);
        QStringList heads;
        for (int c = 0; c < 6; ++c) heads << table->horizontalHeaderItem(c)->text();
        QCOMPARE(heads, (QStringList{ QStringLiteral("Switch"), QStringLiteral("State"), QStringLiteral("Age"),
                                      QStringLiteral("Set"), QStringLiteral("Clear"), QStringLiteral("Meaning") }));
        for (int i = 0; i < APPLE2VID_SW_COUNT; ++i) {
            QCOMPARE(table->item(i, 0)->text(), QString::fromUtf8(sw[i].name));
            QCOMPARE(table->item(i, 1)->text(), sw[i].on ? QStringLiteral("on") : QStringLiteral("off"));
            bool ok = false;
            table->item(i, 2)->text().toInt(&ok);
            QVERIFY(ok);                                    /* the age */
            QCOMPARE(table->item(i, 3)->text(), QStringLiteral("$%1").arg(sw[i].set_addr, 4, 16, QLatin1Char('0')).toUpper());
            QCOMPARE(table->item(i, 4)->text(), QStringLiteral("$%1").arg(sw[i].clr_addr, 4, 16, QLatin1Char('0')).toUpper());
            QVERIFY(!table->item(i, 5)->text().isEmpty());
        }
        shot(QStringLiteral("video-switches"));
        /* Break on change: read+write breakpoints on both addresses */
        auto *b = qobject_cast<QPushButton *>(table->cellWidget(2, 6));
        QVERIFY(b);
        QCOMPARE(b->text(), QStringLiteral("Break on change"));
        QTest::mouseClick(b, Qt::LeftButton);
        apple2debug_breakpoint bps[16];
        const int n = apple2debug_breakpoint_list(m_d, bps, 16);
        QCOMPARE(n, 2);
        QSet<int> addrs;
        for (int i = 0; i < n; i++) {
            QVERIFY((bps[i].type & APPLE2DEBUG_BP_READ) && (bps[i].type & APPLE2DEBUG_BP_WRITE));
            QCOMPARE(bps[i].start, bps[i].end);
            addrs << bps[i].start;
        }
        QCOMPARE(addrs, (QSet<int>{ (int)sw[2].set_addr, (int)sw[2].clr_addr }));
        apple2vid_tab_free(ref);
        apple2debug_breakpoint_clear(m_d);
        sections->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    /* The Colours and Timing sections. */
    void videoColoursAndTiming()
    {
        decodedText40();
        QWidget *pic = vw<QWidget>("videoPicture");
        auto *sections = vw<QTabWidget>("videoSections");
        QCOMPARE(sections->tabText(2), QStringLiteral("Colours"));
        QCOMPARE(sections->tabText(3), QStringLiteral("Timing"));
        /* pin a pixel so one entry is the inspected one */
        clickPicture(pic, QPoint(3 * 14 * 2 + 12, 2 * 8 * 2 + 6));
        sections->setCurrentIndex(2);
        auto *colours = vw<QTableWidget>("videoColours");
        QVERIFY(colours);
        QTRY_VERIFY_WITH_TIMEOUT(colours->rowCount() >= 2, 2000);          /* text: black and white at least */
        long total = 0;
        bool inspected = false;
        for (int r = 0; r < colours->rowCount(); ++r) {
            QVERIFY(colours->item(r, 2)->text().startsWith(QLatin1Char('#')));
            total += colours->item(r, 3)->text().section(QLatin1Char(' '), 0, 0).toLong();
            inspected |= !colours->item(r, 4)->text().isEmpty();
        }
        QCOMPARE(total, (long)APPLE2VID_W * APPLE2VID_H);
        QVERIFY(inspected);
        shot(QStringLiteral("video-colours"));
        sections->setCurrentIndex(3);
        auto *timing = vw<QLabel>("videoTiming");
        QVERIFY(timing);
        QTRY_VERIFY_WITH_TIMEOUT(!timing->text().isEmpty() && timing->text() != QLatin1String("No timing yet"), 2000);
        shot(QStringLiteral("video-timing"));
        sections->setCurrentIndex(0);
        clickPicture(pic, QPoint(3 * 14 * 2 + 12, 2 * 8 * 2 + 6));     /* unpin */
        pickText(vw<QComboBox>("videoZoom"), QStringLiteral("1×"));
        m_tabs->setCurrentIndex(0);
    }

    /* The Memory tab's bank chooser: main and aux differ at one address; Go To takes a prefix. */
    void memoryBank()
    {
        if (!apple2debug_bank_valid(m_d, APPLE2_BANK_AUX)) QSKIP("this machine has no aux RAM");
        apple2debug_write_bank(m_d, APPLE2_BANK_MAIN, 0x0910, 0xD7);
        apple2debug_write_bank(m_d, APPLE2_BANK_AUX, 0x0910, 0x3C);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *bank = m_tabs->widget(2)->findChild<QComboBox *>(QStringLiteral("memBank"));
        QVERIFY(bank);
        QCOMPARE(bank->count(), (int)APPLE2_BANK_COUNT);
        for (int b = 0; b < bank->count(); ++b) QCOMPARE(bank->itemText(b), QString::fromUtf8(apple2debug_bank_name(b)));
        QCOMPARE(bank->currentIndex(), (int)APPLE2_BANK_CURRENT);
        QCOMPARE(bank->itemText(0), QStringLiteral("As the CPU sees it"));
        QLineEdit *go = entryIs(QStringLiteral("$C000, label or lo-hi"));
        QVERIFY(go);
        typeInto(go, QStringLiteral("0910"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0910: ")));
        bank->setCurrentIndex(APPLE2_BANK_MAIN);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0910: D7 ")));
        bank->setCurrentIndex(APPLE2_BANK_AUX);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0910: 3C ")));
        QVERIFY(!memView()->toPlainText().contains(QStringLiteral("$0910: D7 ")));
        /* the header names the bank */
        bool named = false;
        for (QLabel *l : m_tabs->widget(2)->findChildren<QLabel *>())
            named |= l->text().contains(QString::fromUtf8(apple2debug_bank_name(APPLE2_BANK_AUX)));
        QVERIFY(named);
        shot(QStringLiteral("memory-aux"));

        /* Write goes to the chosen bank */
        QLineEdit *addr = entryIs(QStringLiteral("$C000")), *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("0911"));
        typeInto(val, QStringLiteral("$99"));
        uint8_t b = 0;
        apple2debug_read_bank(m_d, APPLE2_BANK_AUX, 0x0911, &b, 1);
        QCOMPARE((int)b, 0x99);
        apple2debug_read_bank(m_d, APPLE2_BANK_MAIN, 0x0911, &b, 1);
        QVERIFY(b != 0x99);

        /* Find Next searches the chosen bank */
        const uint8_t pat[4] = { 0xA7, 0x5E, 0x11, 0x2B };
        for (int i = 0; i < 4; ++i) apple2debug_write_bank(m_d, APPLE2_BANK_AUX, (uint16_t)(0x0A40 + i), pat[i]);
        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        typeInto(go, QStringLiteral("0900"));
        find->setText(QStringLiteral("A7 5E 11 2B"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0A40: A7 5E 11 2B")));
        bank->setCurrentIndex(APPLE2_BANK_MAIN);                            /* not there in main */
        typeInto(go, QStringLiteral("0900"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));

        /* Go To with a prefix switches the chooser; a bare address keeps it */
        bank->setCurrentIndex(APPLE2_BANK_CURRENT);
        typeInto(go, QStringLiteral("aux:$0910"));
        QCOMPARE(bank->currentIndex(), (int)APPLE2_BANK_AUX);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0910: 3C ")));
        typeInto(go, QStringLiteral("main:0910"));
        QCOMPARE(bank->currentIndex(), (int)APPLE2_BANK_MAIN);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$0910: D7 ")));
        typeInto(go, QStringLiteral("0910"));
        QCOMPARE(bank->currentIndex(), (int)APPLE2_BANK_MAIN);

        /* a watch's Memory action uses the watch's own prefix */
        apple2watch_clear();
        apple2watch_add("aux:$0910");
        m_tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *wmem = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (QPushButton *p : m_tabs->currentWidget()->findChildren<QPushButton *>())
                    if (p->text() == QLatin1String("Memory")) wmem = p;
                return wmem != nullptr;
            }(), 2000);
        QTest::mouseClick(wmem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        QCOMPARE(bank->currentIndex(), (int)APPLE2_BANK_AUX);
        apple2watch_clear();

        bank->setCurrentIndex(APPLE2_BANK_CURRENT);
        apple2debug_write_bank(m_d, APPLE2_BANK_AUX, 0x0910, 0);
        m_tabs->setCurrentIndex(0);
    }

    /* FujiNet: empty without the runtime, and says so. */
    void fujinetTab()
    {
        m_tabs->setCurrentIndex(9);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet"));
        QVERIFY(note);
        QVERIFY(m_tabs->currentWidget()->findChild<QPushButton *>()->text() == QLatin1String("Clear"));
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
