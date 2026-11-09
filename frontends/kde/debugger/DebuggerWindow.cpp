/*
 * Debugger window (Qt6 Widgets) over the 6502 debugger engine, via
 * core/include/apple2debug.h. Mirrors the GNOME one tab for tab, and
 * follows the astrocade, SMS, MSX, Atari 7800 and ADAM KDE debuggers.
 *
 * Tabs, in this order (APPLE2_DEBUGGER_TAB picks the one a window opens on,
 * by index or by name -- 0 console, 1 disassembly, 2 memory, 3 breakpoints,
 * 4 watches, 5 slots, 6 trace, 7 history, 8 video, 9 fujinet -- or by the
 * start of a label): the Console (the engine's command line, with a history
 * and Tab completion), the Disassembly (click a line to select it for Run to
 * Cursor; click its left edge, double-click it or press F9 for its
 * breakpoint), Memory (a hex dump, Go to a label / address / range, a hex or
 * text search), Breakpoints (execute, read and write, with a condition),
 * Watches (a value list, Break on change), Slots & I/O (the cards, the
 * memory map, the soft switches), Trace and History (the engine's
 * instruction ring: live, and read at a stop), Video (the pages of video
 * memory as pictures) and FujiNet (the SmartPort packets on the SLIP link).
 * The registers sit in a strip under the tabs, on every one of them. Any
 * tab can be popped out into a window of its own (APPLE2_POPOUT names one at
 * start). The toolbar carries the stepping controls, symbol loading and
 * Save; the family's keys apply (F5 run/stop, F7 step, F8 step over,
 * Shift+F8 step out, Shift+F7 frame, F9 breakpoint, F12 close).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches, which lets the machine run on.
 *
 * A short timer polls the engine's generation and refreshes the tab showing
 * when it moved; while the machine runs, the live tabs refresh every
 * 400 ms. The stop callback fires on the emulator thread, so it only counts
 * stops, which tells the console about one that ended between two polls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "../help/HelpWindow.h"

#include "apple2memtools.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <atomic>
#include <cstdio>
#include <cstring>

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define BP_MAX 256
#define TRACE_MAX 512
#define HIST_SHOWN 2000         /* trace lines shown */
#define RING_MAX 65537
#define CONSOLE_LINES 5000

namespace {

enum {
    TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_SLOTS, TAB_TRACE, TAB_HISTORY, TAB_VIDEO,
    TAB_FUJINET, TAB_COUNT
};

/* The tabs, in order: the names APPLE2_DEBUGGER_TAB takes, their labels
 * (which it takes too), and their Help contexts (apple2help.h). */
const struct { const char *name, *label, *help; } kTabs[TAB_COUNT] = {
    { "console", "Console", "console" },           { "disassembly", "Disassembly", "disassembly" },
    { "memory", "Memory", "memory" },              { "breakpoints", "Breakpoints", "breakpoints" },
    { "watches", "Watches", "watches" },           { "slots", "Slots & I/O", "slots" },
    { "trace", "Trace", "trace" },                 { "history", "History", "history" },
    { "video", "Video", "video" },                 { "fujinet", "FujiNet", "fujinet" },
};

/* The register entries, in the order shown, and the apple2debug_reg each
 * sets. SP is a full address ($0100-$01FF), as the engine keeps it. */
const struct { const char *name; int id, digits; } kRegs[6] = {
    { "PC", APPLE2_REG_PC, 4 }, { "SP", APPLE2_REG_SP, 4 }, { "A", APPLE2_REG_A, 2 },
    { "X", APPLE2_REG_X, 2 },   { "Y", APPLE2_REG_Y, 2 },   { "P", APPLE2_REG_P, 2 },
};

/* The flags the engine can set, in the order of P's bits 7..0 (bit 5 is
 * unused and always 1). */
const struct { const char *name; int id; } kFlags[7] = {
    { "N", APPLE2_FLAG_N }, { "V", APPLE2_FLAG_V }, { "B", APPLE2_FLAG_B }, { "D", APPLE2_FLAG_D },
    { "I", APPLE2_FLAG_I }, { "Z", APPLE2_FLAG_Z }, { "C", APPLE2_FLAG_C },
};

const QString kGray = QStringLiteral("color: gray;");

/* The PC's line, a marked memory row and the Run button: the icon's red,
 * white ink. */
QColor accentColor()
{
    return QColor(0xf4, 0x43, 0x36);
}

QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(accentColor());
    f.setForeground(Qt::white);
    return f;
}

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* A toggle takes the checked style (the accent, a box of its own), which is
 * not the native button's: one size for both states and every text,
 * so it never jumps. */
void fixToggleSize(QPushButton *b, const QStringList &texts, int minHeight)
{
    b->ensurePolished();
    QSize s(0, minHeight);
    for (const bool on : { false, true })
        for (const QString &t : texts) {
            b->setChecked(on);
            b->setText(t);
            s = s.expandedTo(b->sizeHint());
        }
    b->setChecked(false);
    b->setText(texts.first());
    b->setFixedSize(s);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A stretch of one line of a view in a format. */
QTextEdit::ExtraSelection spanSelection(QPlainTextEdit *v, int line, int from, int length, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    const QTextBlock block = v->document()->findBlockByNumber(line);
    s.cursor = QTextCursor(block);
    s.cursor.setPosition(block.position() + from);
    s.cursor.setPosition(block.position() + from + length, QTextCursor::KeepAnchor);
    s.format = fmt;
    return s;
}

/* A status line that elides instead of widening the window. */
class ElidedLabel : public QLabel {
public:
    ElidedLabel() { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = contentsRect();
        p.setPen(palette().color(foregroundRole()));
        p.drawText(r, (alignment() & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter,
                   fontMetrics().elidedText(text(), Qt::ElideRight, r.width()));
    }
};

/* APPLE2_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("i/o" ...). */
int tabFromEnv(const char *var = "APPLE2_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(var).trimmed();
    if (tab.isEmpty()) return -1;
    if (tab.compare(QLatin1String("io"), Qt::CaseInsensitive) == 0) return TAB_SLOTS;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

/* The family's keys: F5 run/stop, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F7: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

QPointer<DebuggerWindow> s_win;

/* Every park of the machine, counted on the emulator thread (all the
 * callback does), so a stop that comes and goes between two polls -- a run
 * that hits a breakpoint at once -- still reaches the console. */
std::atomic<unsigned> g_stops{ 0 };

void countStop(void *, int, uint16_t)
{
    g_stops.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, apple2session *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* showEvent attaches, which stops the machine */
    s_win->QMainWindow::show();
    s_win->raise();
    s_win->activateWindow();
}

void DebuggerWindow::toggleFor(QWidget *parent, apple2session *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

void DebuggerWindow::show(QWidget *parent, apple2session *session)
{
    showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(apple2session *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(apple2session_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    apple2debug_set_stop_callback(m_dbg, countStop, nullptr);
    m_seenStops = g_stops.load(std::memory_order_relaxed);
    setWindowTitle(QStringLiteral("Apple II Debugger"));
    resize(1400, 820);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    m_pages[TAB_CONSOLE] = buildConsole();
    m_pages[TAB_DISASM] = buildDisasm();
    m_pages[TAB_MEMORY] = buildMemory();
    m_pages[TAB_BREAKS] = buildBreaks();
    m_pages[TAB_WATCHES] = buildWatches();
    m_pages[TAB_SLOTS] = buildSlots();
    m_pages[TAB_TRACE] = buildHist(0);
    m_pages[TAB_HISTORY] = buildHist(1);
    m_pages[TAB_VIDEO] = buildVideo();
    m_pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i) {
        /* a page's place in the tab widget is not its identity once tabs move
         * into windows of their own: each carries its own number */
        m_pages[i]->setProperty("tab", i);
        m_tabs->addTab(m_pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    }
    int tab = tabFromEnv();
    {
        /* APPLE2_DEBUGGER_VIEW: land on a video page (an index into
         * apple2debug_view_name) instead of the console */
        bool ok = false;
        const int idx = qEnvironmentVariableIntValue("APPLE2_DEBUGGER_VIEW", &ok);
        if (ok && idx >= 0 && idx < APPLE2VIEW_COUNT) {
            m_viewPick->setCurrentIndex(idx);
            if (tab < 0) tab = TAB_VIDEO;
        }
    }
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts (apple2help.h), one per tab */
    auto setHelpContext = [this](int page) {
        if (page >= 0 && page < TAB_COUNT)
            setProperty("apple2HelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].help)));
    };
    setHelpContext(currentTab());
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { apple2HelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        const int page = currentTab();
        setHelpContext(page);
        if (page == TAB_HISTORY) m_hSeen[1] = -1;
        refreshPage(page, false);
        if (page == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    /* for captures and tests: APPLE2_WATCH adds a watch, APPLE2_WATCH_BREAK a
     * break-on-change over one, APPLE2_MEM_GOTO shows a range in Memory,
     * APPLE2_HISTORY records the trace and APPLE2_POPOUT peels a tab off */
    {
        const QByteArray wt = qgetenv("APPLE2_WATCH"), wb = qgetenv("APPLE2_WATCH_BREAK"),
                         mg = qgetenv("APPLE2_MEM_GOTO");
        uint16_t lo, hi;
        if (!wt.isEmpty()) apple2watch_add(wt.constData());
        if (!wb.isEmpty() && apple2sym_parse_range(m_dbg, wb.constData(), &lo, &hi)) {
            char err[160] = "";
            apple2debug_breakpoint_add(m_dbg, APPLE2DEBUG_BP_WRITE, lo, hi, "value != old", err, sizeof err);
        }
        if (!mg.isEmpty() && apple2sym_parse_range(m_dbg, mg.constData(), &lo, &hi)) showInMemory(lo, hi);
        if (tabFromEnv("APPLE2_POPOUT") >= 0) popOut(tabFromEnv("APPLE2_POPOUT"));
    }

    /* the timer runs only while the window is shown (showEvent/hideEvent) */
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

/* Only at exit, with the main window: the session may already be gone, so
 * the engine is touched only if this window attached it. */
DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;       /* a peeled-off tab's window goes with the debugger */
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *w = win;
            win = nullptr;
            w->close();
        }
    if (m_attached) {
        apple2debug_detach(m_dbg);
        m_attached = false;
    }
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    /* Run is a checkable button, checked while the machine is stopped: its
     * stylesheet gives that the accent. */
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    m_runBtn->setCheckable(true);
    m_runBtn->setStyleSheet(QStringLiteral("QPushButton:checked { background:%1; color:white; }")
                                .arg(accentColor().name()));
    auto *step = add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"),
                     [this] { stepAnd([this] { apple2debug_step(m_dbg); }); });
    add(QStringLiteral("Over (F8)"), QStringLiteral("Over a JSR or BRK"),
        [this] { stepAnd([this] { apple2debug_step_over(m_dbg); }); });
    add(QStringLiteral("Out (Shift+F8)"), QStringLiteral("On to the RTS or RTI"),
        [this] { stepAnd([this] { apple2debug_step_out(m_dbg); }); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Scanline"), QStringLiteral("On to the next video scanline"),
        [this] { stepAnd([this] { apple2debug_scanline(m_dbg, 1); }); });
    add(QStringLiteral("Frame (Shift+F7)"), QStringLiteral("On to the start of the next frame"),
        [this] { stepAnd([this] { apple2debug_frame(m_dbg, 1); }); });
    fixToggleSize(m_runBtn, { QStringLiteral("Stop (F5)"), QStringLiteral("Run (F5)") }, step->sizeHint().height());

    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    m_followPc->setFocusPolicy(Qt::NoFocus);
    m_followPc->setToolTip(QStringLiteral("Keep the Disassembly on the PC"));
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    h->addWidget(m_followPc);

    m_status = new ElidedLabel;
    m_status->setAlignment(Qt::AlignRight);
    m_status->setStyleSheet(kGray);
    m_status->setContentsMargins(8, 0, 8, 0);
    h->addWidget(m_status, 1);

    auto *sym = new QPushButton(QStringLiteral("Load Symbols…"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from a .sym, .lbl or .map file"));
    connect(sym, &QPushButton::clicked, this, [this] { loadSymbols(); });
    h->addWidget(sym);

    auto *saveBtn = new QPushButton(QStringLiteral("Save…"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    menu->addAction(QStringLiteral("Disassembly ($0000-$FFFF as mapped)"), this,
                    [this] { save("dis", QStringLiteral("Disassembly"), QStringLiteral("disassembly.asm")); });
    menu->addAction(QStringLiteral("Memory (the 64K the CPU sees)"), this,
                    [this] { save("mem", QStringLiteral("Memory"), QStringLiteral("memory.bin")); });
    menu->addAction(QStringLiteral("CPU registers and soft switches"), this,
                    [this] { save("regs", QStringLiteral("Registers"), QStringLiteral("registers.txt")); });
    saveBtn->setMenu(menu);
    h->addWidget(saveBtn);
    auto *pop = new QPushButton(QStringLiteral("Pop Out"));
    pop->setFocusPolicy(Qt::NoFocus);
    pop->setToolTip(QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"));
    connect(pop, &QPushButton::clicked, this, [this] { popOut(currentTab()); });
    h->addWidget(pop);
    h->addWidget(apple2HelpButton(QString()));
    return bar;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command. Numbers: $C000 or 0xC000 "
                                          "for hex, bare digits are decimal.\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, mem, poke, runto, disasm, regs, "
                                               "io ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    v->addWidget(m_conOut, 1);
    v->addWidget(m_conIn);
    return w;
}

/* The registers, under the tabs and so on every one of them: PC SP A X Y P
 * (Enter sets one), then the flags and the beam. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto *regs = new QHBoxLayout;
    regs->setSpacing(8);
    for (int i = 0; i < 6; ++i) {
        auto *l = new QLabel(QString::fromUtf8(kRegs[i].name));
        l->setStyleSheet(kGray);
        regs->addWidget(l);
        m_reg[i] = new QLineEdit;
        setMono(m_reg[i]);
        m_reg[i]->setMaxLength(12);
        setDigits(m_reg[i], kRegs[i].digits);
        m_reg[i]->setToolTip(QStringLiteral("Enter sets it (hex, or an expression; only while stopped)"));
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] { setReg(i); });
        regs->addWidget(m_reg[i]);
    }
    regs->addStretch(1);
    v->addLayout(regs);

    /* N V - B D I Z C, as in P; bit 5 is shown, not set */
    auto *flags = new QHBoxLayout;
    int idx = 0;
    for (const char *name : { "N", "V", "-", "B", "D", "I", "Z", "C" }) {
        auto *c = new QCheckBox(QString::fromUtf8(name));
        if (name[0] == '-') {
            c->setEnabled(false);
            c->setToolTip(QStringLiteral("Bit 5 of P: unused, always 1"));
            m_flagRo = c;
        } else {
            const int at = idx++;
            m_flag[at] = c;
            connect(c, &QCheckBox::clicked, this, [this, at](bool on) { setFlag(kFlags[at].id, on); });
        }
        flags->addWidget(c);
    }
    flags->addSpacing(16);
    m_beam = new QLabel;
    setMono(m_beam);
    m_beam->setStyleSheet(kGray);
    flags->addWidget(m_beam, 1);
    v->addLayout(flags);
    return w;
}

/* The dump, Go to (a label, address or range, which it marks), the 4K paging,
 * the write control, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    v->addWidget(titleLabel(QStringLiteral("Memory (as the CPU sees it through the bank map)")));
    m_mem = monoView();
    v->addWidget(m_mem, 1);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$C000, label or lo-hi"), 20);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    for (int dir = -1; dir <= 1; dir += 2) {
        auto *b = new QToolButton;
        b->setArrowType(dir < 0 ? Qt::UpArrow : Qt::DownArrow);
        b->setToolTip(dir < 0 ? QStringLiteral("Back 4K") : QStringLiteral("On 4K"));
        connect(b, &QToolButton::clicked, this, [this, dir] {
            m_memTop = qBound(0, m_memTop + dir * MEM_ROWS * 16, 0x10000 - MEM_ROWS * 16);
            refreshMem();
        });
        nav->addWidget(b);
    }
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Write")));
    m_memAddr = smallEntry(QStringLiteral("$C000"), 10);
    m_memVal = smallEntry(QStringLiteral("$00"), 5);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addStretch(1);
    v->addLayout(nav);
    v->addWidget(buildSearchBar());
    return w;
}

QWidget *DebuggerWindow::buildSearchBar()
{
    auto *bar = new QWidget;
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (A9 00) or text, one byte per character"));
    row->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    setMono(m_srchEntry);
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text to find"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    row->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    row->addWidget(next);
    row->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    row->addWidget(m_srchNote);
    return bar;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

/* A page of video memory decoded to pixels: what the CPU sees there now,
 * which is not necessarily what is on screen. */
QWidget *DebuggerWindow::buildVideo()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_viewFb.assign((size_t)APPLE2VIEW_WIDTH * APPLE2VIEW_HEIGHT, 0);
    auto *row = new QHBoxLayout;
    m_viewPick = new QComboBox;
    /* the page list comes from the engine, so a view added there shows up
     * here with no edit */
    for (int i = 0; apple2debug_view_name(i); i++) m_viewPick->addItem(QString::fromUtf8(apple2debug_view_name(i)));
    connect(m_viewPick, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refreshVideo(); });
    row->addWidget(new QLabel(QStringLiteral("Page")));
    row->addWidget(m_viewPick);
    m_viewNote = new QLabel;
    m_viewNote->setStyleSheet(kGray);
    row->addWidget(m_viewNote, 1);
    v->addLayout(row);
    m_viewPic = new QLabel;
    m_viewPic->setFixedSize(APPLE2VIEW_WIDTH * 2, APPLE2VIEW_HEIGHT * 2);
    m_viewPic->setStyleSheet(QStringLiteral("background: black;"));
    v->addWidget(m_viewPic, 0, Qt::AlignLeft | Qt::AlignTop);
    v->addWidget(hintLabel(QStringLiteral("A memory inspector: it decodes the page the CPU sees now, which is how "
                                          "to find the buffer a program draws into before it flips to it.")));
    v->addStretch(1);
    /* the page can outgrow the window on a short screen: it scrolls */
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(w);
    return scroll;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(QStringLiteral("font-weight: bold;"));
    v->addWidget(m_bpStop);

    auto *top = new QHBoxLayout;
    top->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; the others after the "
                                            "access. Read and Write see data accesses only. Breakpoints are kept "
                                            "while the debugger is closed, but only act while it is open.")), 1);
    v->addLayout(top);

    m_bpTable = new QTableWidget(0, 4);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Breakpoint"), QStringLiteral("Hits"),
                                           QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        apple2debug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("Read/Write") });
    m_bpType->setToolTip(QStringLiteral("Read and Write watch the soft switches too ($C000-$C0FF)"));
    m_bpStart = smallEntry(QStringLiteral("start ($0300 or label)"), 22);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. a == 5 && value == $FF)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    connect(clearHits, &QPushButton::clicked, this, [this] {
        apple2debug_breakpoint_clear_hits(m_dbg);
        resync();
    });
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        apple2debug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    setMono(m_wtEntry);
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or lo-hi"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(); });
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(); });
    row->addWidget(m_wtEntry, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_wtTable = new QTableWidget(0, 2);
    m_wtTable->setHorizontalHeaderLabels({ QStringLiteral("Watch"), QString() });
    m_wtTable->verticalHeader()->setVisible(false);
    m_wtTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_wtTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_wtTable->setColumnWidth(1, 350);
    m_wtTable->verticalHeader()->setDefaultSectionSize(34);
    m_wtTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_wtTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_wtTable->setFocusPolicy(Qt::NoFocus);
    v->addWidget(m_wtTable, 1);
    v->addWidget(hintLabel(QStringLiteral("The values are read at a stop and show - while the machine runs. A watch "
                                          "is kept as text, so a label that loads later starts resolving. "
                                          "Break on change stops after a write that alters the value.")));
    return w;
}

/* The slots and their cards, the memory map as the CPU sees it now, and the
 * soft switches as the engine words them. */
QWidget *DebuggerWindow::buildSlots()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto table = [](const QStringList &heads) {
        auto *t = new QTableWidget(0, (int)heads.size());
        t->setHorizontalHeaderLabels(heads);
        t->verticalHeader()->setVisible(false);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        t->horizontalHeader()->setStretchLastSection(true);
        t->setSelectionMode(QAbstractItemView::NoSelection);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setFocusPolicy(Qt::NoFocus);
        setMono(t);
        return t;
    };
    m_slotTable = table({ QStringLiteral("Slot"), QStringLiteral("Card"), QStringLiteral("FujiNet") });
    m_mapTable = table({ QStringLiteral("Range"), QStringLiteral("R/W"), QStringLiteral("Region") });
    auto *top = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    left->addWidget(titleLabel(QStringLiteral("Slots")));
    left->addWidget(m_slotTable, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(titleLabel(QStringLiteral("Memory map (as the CPU sees it now)")));
    right->addWidget(m_mapTable, 1);
    top->addLayout(left, 2);
    top->addLayout(right, 3);
    v->addLayout(top, 3);
    v->addWidget(titleLabel(QStringLiteral("Soft switches")));
    m_io = monoView();
    v->addWidget(m_io, 2);
    return w;
}

/* Trace (idx 0) reads the instruction ring live; History (idx 1) reads it at
 * a stop. They share the one ring and recorder, so the two Record buttons
 * follow each other. */
QWidget *DebuggerWindow::buildHist(int idx)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *bar = new QHBoxLayout;

    m_hRec[idx] = new QPushButton(QStringLiteral("Record"));
    m_hRec[idx]->setCheckable(true);
    m_hRec[idx]->setFocusPolicy(Qt::NoFocus);
    m_hRec[idx]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    m_hRec[idx]->setStyleSheet(QStringLiteral("QPushButton:checked { background:%1; color:white; }")
                                   .arg(accentColor().name()));
    fixToggleSize(m_hRec[idx], { QStringLiteral("Record") }, m_hRec[idx]->sizeHint().height());
    connect(m_hRec[idx], &QPushButton::toggled, this, [this](bool on) {
        if (m_hSync) return;
        apple2debug_trace_enable(m_dbg, on);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hFilter[idx] = new QComboBox;
    m_hFilter[idx]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                               QStringLiteral("In range") });
    m_hRange[idx] = new QLineEdit;
    setMono(m_hRange[idx]);
    m_hRange[idx]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    connect(m_hFilter[idx], QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, idx](int) { refreshHist(idx, true); });
    connect(m_hRange[idx], &QLineEdit::returnPressed, this, [this, idx] { refreshHist(idx, true); });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        apple2debug_trace_clear(m_dbg);
        for (int i = 0; i < 2; ++i) refreshHist(i, true);
    });
    m_hNote[idx] = new QLabel;
    m_hNote[idx]->setStyleSheet(kGray);
    m_hNote[idx]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[idx]);
    bar->addWidget(m_hFilter[idx]);
    bar->addWidget(m_hRange[idx], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[idx], 1);
    v->addLayout(bar);
    m_hText[idx] = monoView();
    v->addWidget(m_hText[idx], 1);
    return w;
}

/* The packets left (a note and Clear over them), the card right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("SmartPort packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        apple2debug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("SmartPort card")));
    m_fujiState = monoView();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr = -1;
    const bool stopped = apple2debug_is_stopped(m_dbg) != 0;
    reason[0] = '\0';
    apple2debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    QString text = QStringLiteral("Running");
    if (stopped) {
        const QString why = stripControl(reason);
        text = why.isEmpty() || why == QLatin1String("stopped") ? QStringLiteral("Stopped")
                                                                : QStringLiteral("Stopped: ") + why;
        /* where, by name when it has one */
        uint16_t off = 0;
        const char *sym = addr >= 0 ? apple2debug_symbol_at(m_dbg, (uint16_t)addr, &off) : nullptr;
        if (sym) text += QStringLiteral("  (%1%2)").arg(QString::fromUtf8(sym),
                                                        off ? QStringLiteral("+%1").arg(off) : QString());
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
        m_status->update();
    }
    const QString run = stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)");
    if (m_runBtn->text() != run) m_runBtn->setText(run);
    if (m_runBtn->isChecked() != stopped) m_runBtn->setChecked(stopped);
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus()) return;
    if (e->text() != text) e->setText(text);
}

void DebuggerWindow::refreshCpu()
{
    apple2debug_cpu c;
    apple2debug_cpu_get(m_dbg, &c);
    const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.p };
    for (int i = 0; i < 6; ++i)
        setEntry(m_reg[i], hx((unsigned)vals[i] & (kRegs[i].digits == 4 ? 0xFFFF : 0xFF), kRegs[i].digits));
    const int flags[7] = { c.nf, c.vf, c.bf, c.df, c.if_, c.zf, c.cf };
    for (int i = 0; i < 7; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_flagRo->setChecked((c.p & 0x20) != 0);
    m_beam->setText(QStringLiteral("line %1   frame %2   %3 cycles   %4%5")
                        .arg(c.line, 3).arg(c.frame).arg(c.cycles)
                        .arg(c.is_65c02 ? QStringLiteral("65C02") : QStringLiteral("6502"),
                             c.jammed ? QStringLiteral("   JAMMED") : QString()));
}

void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    memset(mem, 0, sizeof mem);
    apple2debug_read(m_dbg, (uint16_t)m_memTop, mem, (int)sizeof mem);
    QString t;
    t.reserve(MEM_ROWS * 80);
    t += QStringLiteral("       ");
    for (int col = 0; col < 16; ++col) t += hx((unsigned)col, 1) + QStringLiteral("  ");
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *r = mem + row * 16;
        t += QStringLiteral("\n$%1: ").arg(hx((unsigned)(m_memTop + row * 16), 4));
        for (int col = 0; col < 16; ++col) t += hx(r[col], 2) + QLatin1Char(' ');
        t += QLatin1Char(' ');
        /* Apple II text carries the high bit; mask it before deciding whether
         * a byte is printable, or nothing ever looks like text */
        for (int col = 0; col < 16; ++col) {
            const uint8_t ch = r[col] & 0x7F;
            t += QLatin1Char(ch >= 0x20 && ch < 0x7F ? (char)ch : '.');
        }
    }
    setText(m_mem, t);

    /* the range Go to or a search found stays marked across refreshes: each
     * row's bytes in the hex and in the text column */
    QList<QTextEdit::ExtraSelection> sel;
    if (m_hl) {
        const int lo = qMax((int)m_hlLo, m_memTop), hi = qMin((int)m_hlHi, m_memTop + MEM_ROWS * 16 - 1);
        for (int row = (lo - m_memTop) / 16; lo <= hi && row <= (hi - m_memTop) / 16; ++row) {
            const int first = qMax(lo - m_memTop - row * 16, 0), last = qMin(hi - m_memTop - row * 16, 15);
            sel << spanSelection(m_mem, 1 + row, 7 + first * 3, (last - first) * 3 + 2, accentFormat());
            sel << spanSelection(m_mem, 1 + row, 7 + 48 + 1 + first, last - first + 1, accentFormat());
        }
    }
    m_mem->setExtraSelections(sel);
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static apple2debug_line lines[DISASM_WINDOW];
    const int rows = visibleRows();
    if (m_followPc->isChecked()) {
        apple2debug_cpu c;
        apple2debug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)apple2debug_row_address(m_dbg, (uint16_t)c.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int n = apple2debug_disassemble(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);

    m_lineAddr.clear();
    QString text;
    char buf[128];
    struct Span { int line, from, length; };
    std::vector<Span> grays;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        /* the gutter: a breakpoint dot (hollow when disabled), then the
         * PC's arrow */
        QString ln = lines[i].has_breakpoint ? (lines[i].breakpoint_enabled ? QStringLiteral("●") : QStringLiteral("○"))
                                             : QStringLiteral(" ");
        snprintf(buf, sizeof buf, "%c%04X  %-8.8s ", lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes);
        ln += QString::fromUtf8(buf);
        const int labelFrom = (int)ln.size();
        char label[48];
        snprintf(label, sizeof label, "%-*.*s ", labelWidth, labelWidth, lines[i].label);
        ln += QString::fromUtf8(label);
        if (lines[i].label[0] && i != pcLine)
            grays.push_back({ i, labelFrom, (int)strlen(lines[i].label) < labelWidth ? (int)strlen(lines[i].label)
                                                                                      : labelWidth });
        snprintf(buf, sizeof buf, "%-16s", lines[i].disasm);
        ln += QString::fromUtf8(buf);
        if (lines[i].comment[0]) {
            const QString comment = QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
            if (i != pcLine) grays.push_back({ i, (int)ln.size(), (int)comment.size() });
            ln += comment;
        }
        if (i) text += QLatin1Char('\n');
        text += ln;
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    /* The PC's line is the accent bar: the block's own background, not an
     * extra selection -- a full-width selection paints over the gutter's
     * red dot, a block background lies under it. */
    if (pcLine >= 0 && pcLine < n) {
        QTextCursor cur(m_disasm->document()->findBlockByNumber(pcLine));
        QTextBlockFormat bf;
        bf.setBackground(accentColor());
        cur.setBlockFormat(bf);
        cur.select(QTextCursor::LineUnderCursor);
        QTextCharFormat cf;
        cf.setForeground(Qt::white);
        if (lines[pcLine].address == m_selected) {
            cf.setFontUnderline(true);
            cf.setFontWeight(QFont::Bold);
        }
        cur.mergeCharFormat(cf);
    }

    QList<QTextEdit::ExtraSelection> sel;
    /* labels and comments in the system's grey */
    QTextCharFormat grey;
    grey.setForeground(m_disasm->palette().color(QPalette::Disabled, QPalette::Text));
    for (const Span &g : grays) sel << spanSelection(m_disasm, g.line, g.from, g.length, grey);
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint) {
            QTextCharFormat f;
            if (lines[i].breakpoint_enabled) {
                f.setForeground(i == pcLine ? QColor(Qt::white) : QColor(0xe0, 0x1b, 0x24));
                f.setFontWeight(QFont::Bold);
            } else {
                f.setForeground(QColor(0x9a, 0x99, 0x96));
            }
            sel << spanSelection(m_disasm, i, 0, 1, f);
        }
        /* the selected line underlined (the PC's is done above) */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextCharFormat f;
            f.setFontUnderline(true);
            f.setFontWeight(QFont::Bold);
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format = f;
            sel << s;
        }
    }
    m_disasm->setExtraSelections(sel);
}

/* Decode whichever video page the combo names. This shows a PAGE, not what
 * the machine is currently displaying -- which is the point: it is how you
 * see the buffer a program is drawing into before it flips to it. */
void DebuggerWindow::refreshVideo()
{
    const int sel = m_viewPick->currentIndex();
    if (sel < 0) return;
    if (apple2debug_render_view(m_dbg, (apple2debug_view)sel, m_viewFb.data()) != 0) return;
    m_viewNote->setText(QStringLiteral("at $%1").arg(hx(apple2debug_view_base((apple2debug_view)sel), 4)));
    /* Format_RGB32 ignores the top octet, so the engine's pixels need no
     * conversion; copy() because the QImage is a view over m_viewFb */
    const QImage img((const uchar *)m_viewFb.data(), APPLE2VIEW_WIDTH, APPLE2VIEW_HEIGHT, APPLE2VIEW_WIDTH * 4,
                     QImage::Format_RGB32);
    /* nearest-neighbour on purpose: this is a pixel inspector, so smoothing
     * would hide what is being inspected */
    m_viewPic->setPixmap(QPixmap::fromImage(img.copy().scaled(APPLE2VIEW_WIDTH * 2, APPLE2VIEW_HEIGHT * 2,
                                                              Qt::IgnoreAspectRatio, Qt::FastTransformation)));
}

void DebuggerWindow::refreshBps()
{
    static apple2debug_breakpoint bps[BP_MAX];
    const int n = apple2debug_breakpoint_list(m_dbg, bps, BP_MAX);

    if (apple2debug_is_stopped(m_dbg)) {
        char reason[160];
        int addr = -1;
        reason[0] = '\0';
        apple2debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        const QString why = stripControl(reason);
        m_bpStop->setText(why.isEmpty() || why == QLatin1String("stopped") ? QStringLiteral("Stopped.")
                                                                           : QStringLiteral("Stopped: ") + why);
    } else {
        m_bpStop->setText(QStringLiteral("Running."));
    }

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n").arg(bps[i].id).arg(bps[i].enabled).arg(bps[i].hits)
                   .arg(bps[i].type).arg(bps[i].start).arg(bps[i].end).arg(QString::fromUtf8(bps[i].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        char desc[256];
        apple2bp_describe(m_dbg, &bps[i], desc, sizeof desc);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(stripControl(desc)));
        m_bpTable->setItem(i, 2, new QTableWidgetItem(QString::number(bps[i].hits)));
        for (int col = 1; col < 3; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            apple2debug_breakpoint_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 3, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

/* The slots, the memory map as regions, then the soft switches. The tables
 * are rewritten only when what they say changed. */
void DebuggerWindow::refreshSlots()
{
    static char text[8192];
    static apple2debug_region reg[64];
    static apple2debug_slot cards[16];

    const int ns = apple2debug_slots(m_dbg, cards, 16);
    QString slotSig;
    for (int i = 0; i < ns; ++i)
        slotSig += QStringLiteral("%1|%2|%3\n").arg(cards[i].slot).arg(QString::fromUtf8(cards[i].card)).arg(cards[i].fujinet);
    if (slotSig != m_slotTable->property("sig").toString()) {
        m_slotTable->setProperty("sig", slotSig);
        m_slotTable->setRowCount(ns);
        for (int i = 0; i < ns; ++i) {
            m_slotTable->setItem(i, 0, new QTableWidgetItem(cards[i].slot == 8 ? QStringLiteral("Aux")
                                                                               : QString::number(cards[i].slot)));
            m_slotTable->setItem(i, 1, new QTableWidgetItem(cards[i].card[0] ? QString::fromUtf8(cards[i].card)
                                                                              : QStringLiteral("(empty)")));
            m_slotTable->setItem(i, 2, new QTableWidgetItem(cards[i].fujinet ? QStringLiteral("FujiNet")
                                                                              : QString()));
        }
    }

    const int nr = apple2debug_memory_map(m_dbg, reg, 64);
    QString mapSig;
    for (int i = 0; i < nr; ++i)
        mapSig += QStringLiteral("%1|%2|%3|%4|%5\n").arg(reg[i].lo).arg(reg[i].hi).arg(reg[i].readable)
                      .arg(reg[i].writable).arg(QString::fromUtf8(reg[i].name));
    if (mapSig != m_mapTable->property("sig").toString()) {
        m_mapTable->setProperty("sig", mapSig);
        m_mapTable->setRowCount(nr);
        for (int i = 0; i < nr; ++i) {
            m_mapTable->setItem(i, 0, new QTableWidgetItem(QStringLiteral("$%1-$%2").arg(hx(reg[i].lo, 4), hx(reg[i].hi, 4))));
            m_mapTable->setItem(i, 1, new QTableWidgetItem(QStringLiteral("%1%2")
                                                               .arg(reg[i].readable ? QLatin1Char('r') : QLatin1Char('-'))
                                                               .arg(reg[i].writable ? QLatin1Char('w') : QLatin1Char('-'))));
            m_mapTable->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(reg[i].name)));
        }
    }

    text[0] = '\0';
    apple2debug_io_state(m_dbg, text, sizeof text);
    setText(m_io, stripControl(text));
}

void DebuggerWindow::addWatch()
{
    const QByteArray text = m_wtEntry->text().trimmed().toUtf8();
    if (text.isEmpty()) return;
    if (apple2watch_add(text.constData()) < 0) {
        message(QStringLiteral("The watch list is full"));
        return;
    }
    m_wtEntry->clear();
    refreshWatches(true);
}

/* Break on change: stop after a write over the watch that alters a byte. */
void DebuggerWindow::watchBreak(int index)
{
    uint16_t lo, hi;
    if (!apple2watch_range(m_dbg, index, &lo, &hi)) {
        message(QStringLiteral("That watch does not resolve yet"));
        return;
    }
    char err[160] = "";
    if (apple2debug_breakpoint_add(m_dbg, APPLE2DEBUG_BP_WRITE, lo, hi, "value != old", err, sizeof err) < 0)
        message(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
    else
        message(QStringLiteral("Breaking when it changes"));
    resync();
}

/* One row a watch, its value live (a dash while the machine runs). The rows
 * are rebuilt when the list changed; otherwise only their text moves. */
void DebuggerWindow::refreshWatches(bool force)
{
    const int n = apple2watch_count();
    const unsigned gen = apple2watch_generation();
    QFont mono = m_wtTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    if (force || gen != m_wtGen || m_wtTable->rowCount() != n) {
        m_wtGen = gen;
        m_wtTable->setRowCount(n);
        for (int i = 0; i < n; ++i) {
            auto *item = new QTableWidgetItem;
            item->setFont(mono);
            m_wtTable->setItem(i, 0, item);
            auto *box = new QWidget;
            auto *h = new QHBoxLayout(box);
            h->setContentsMargins(2, 0, 2, 0);
            auto *mem = new QPushButton(QStringLiteral("Memory"));
            mem->setToolTip(QStringLiteral("Show it in the Memory tab"));
            connect(mem, &QPushButton::clicked, this, [this, i] {
                uint16_t lo, hi;
                if (!apple2watch_range(m_dbg, i, &lo, &hi)) {
                    message(QStringLiteral("That watch does not resolve yet"));
                    return;
                }
                if (m_popped[TAB_MEMORY]) m_popped[TAB_MEMORY]->activateWindow();
                else m_tabs->setCurrentWidget(m_pages[TAB_MEMORY]);
                showInMemory(lo, hi);
            });
            auto *brk = new QPushButton(QStringLiteral("Break on change"));
            brk->setToolTip(QStringLiteral("Stop after a write that alters it"));
            connect(brk, &QPushButton::clicked, this, [this, i] { watchBreak(i); });
            auto *rm = new QPushButton(QStringLiteral("Remove"));
            connect(rm, &QPushButton::clicked, this, [this, i] {
                apple2watch_remove(i);
                QTimer::singleShot(0, this, [this] { refreshWatches(true); });
            });
            for (QPushButton *b : { mem, brk, rm }) {
                b->setFocusPolicy(Qt::NoFocus);
                h->addWidget(b);
            }
            m_wtTable->setCellWidget(i, 1, box);
        }
    }
    for (int i = 0; i < n; ++i) {
        char line[256];
        apple2watch_format(m_dbg, i, line, sizeof line);
        const QString text = stripControl(line);
        if (m_wtTable->item(i, 0)->text() != text) m_wtTable->item(i, 0)->setText(text);
    }
}

/* Trace (live) and History (at a stop): the ring newest first, shown
 * oldest first, the newest HIST_SHOWN lines that pass the filter. */
void DebuggerWindow::refreshHist(int idx, bool force)
{
    static apple2debug_trace buf[RING_MAX];
    const bool stopped = apple2debug_is_stopped(m_dbg) != 0;
    const bool on = apple2debug_trace_enabled(m_dbg) != 0;
    const int count = apple2debug_trace_count(m_dbg);

    m_hSync = true;                     /* both Record buttons follow the engine */
    for (int i = 0; i < 2; ++i)
        if (m_hRec[i]->isChecked() != on) m_hRec[i]->setChecked(on);
    m_hSync = false;

    if (force) m_hSeen[idx] = -1;
    /* History reads at a stop (it keeps what it showed while the machine
     * runs); at a stop neither view rebuilds for the same count */
    if (idx == 1 && !stopped && m_hSeen[1] >= 0) return;
    if (stopped && count == m_hSeen[idx]) return;
    m_hSeen[idx] = count;

    uint16_t lo = 0, hi = 0xFFFF;
    int filter = APPLE2DEBUG_TRACE_ALL;
    switch (m_hFilter[idx]->currentIndex()) {
    case 1: filter = APPLE2DEBUG_TRACE_BRANCHES; break;
    case 2:
        if (apple2sym_parse_range(m_dbg, m_hRange[idx]->text().toUtf8().constData(), &lo, &hi))
            filter = APPLE2DEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    const int n = apple2debug_trace_read(m_dbg, buf, RING_MAX);
    QList<int> hit;
    for (int i = 0; i < n && hit.size() < HIST_SHOWN; ++i)
        if (apple2debug_trace_match(&buf[i], filter, lo, hi)) hit.append(i);
    QString t;
    if (!on && n == 0) t = QStringLiteral("Recording is off. Press Record, then let the machine run.");
    char line[256];
    for (int k = (int)hit.size() - 1; k >= 0; --k) {
        apple2debug_trace_format(&buf[hit[k]], line, sizeof line);
        t += QString::fromUtf8(line);
        if (k) t += QLatin1Char('\n');
    }
    setText(m_hText[idx], t, true);
    m_hNote[idx]->setText(on ? QStringLiteral("recording, %1 instructions").arg(n)
                             : n ? QStringLiteral("recording off, %1 instructions").arg(n)
                                 : QStringLiteral("recording off"));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static apple2_fuji_packet p[TRACE_MAX];
    static char state[4096];
    const int n = apple2debug_fujinet_trace(m_dbg, p, TRACE_MAX);
    const int slot = apple2session_fujinet_slot(m_session);
    const QString link = apple2session_fujinet_link(m_session) ? QStringLiteral(", connected")
                                                                : QStringLiteral(", not connected");
    if (!slot)
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences ▸ Slots)."));
    else if (apple2session_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("External FujiNet on the slot %1 SmartPort card, listening at %2%3.")
                                .arg(slot).arg(QString::fromUtf8(apple2session_slip_endpoint(m_session)), link));
    else if (!apple2session_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet's card is in slot %1, but FujiNet is not running.").arg(slot));
    else
        m_fujiNote->setText(QStringLiteral("FujiNet is on the slot %1 SmartPort card%2.").arg(slot).arg(link));
    state[0] = '\0';
    apple2debug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < int(sizeof p[i].data); k++) t += hx(p[i].data[k], 2) + QLatin1Char(' ');
        if (p[i].nbytes > int(sizeof p[i].data)) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no SmartPort packets since the trace was cleared)");
    setText(m_fujiText, t, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshPage(int page, bool live)
{
    switch (page) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_MEMORY: refreshMem(); break;
    case TAB_VIDEO: refreshVideo(); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(false); break;
    case TAB_SLOTS: refreshSlots(); break;
    case TAB_TRACE: refreshHist(0, false); break;
    case TAB_HISTORY: if (!live) refreshHist(1, false); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* The registers show everywhere; then the tab showing and any in a window of
 * its own. */
void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshCpu();
    refreshPage(currentTab(), false);
    for (int t = 0; t < TAB_COUNT; ++t)
        if (m_popped[t]) refreshPage(t, false);
}

/* A page's number, whichever place it holds in the tab widget now. */
int DebuggerWindow::currentTab() const
{
    QWidget *w = m_tabs->currentWidget();
    return w ? w->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then the tab showing follows. */
void DebuggerWindow::resync()
{
    const bool stopped = apple2debug_is_stopped(m_dbg) != 0;
    const unsigned stops = g_stops.load(std::memory_order_relaxed);
    if (stopped && stops != m_seenStops) {
        char reason[160];
        int addr;
        reason[0] = '\0';
        apple2debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        /* a step already says where it is */
        if (reason[0] && strncmp(reason, "step", 4) != 0)
            consoleAppend(QStringLiteral("— %1\n").arg(stripControl(reason)));
    }
    m_seenStops = stops;
    m_wasStopped = stopped;
    /* a new stop re-centres the disassembly on the PC, even if it was
     * scrolled away */
    const uint64_t serial = apple2debug_stop_serial(m_dbg);
    if (serial != m_seenSerial) {
        m_seenSerial = serial;
        if (stopped) {
            m_followPc->setChecked(true);
            m_disasm->verticalScrollBar()->setValue(0);
        }
    }
    m_seenGen = apple2debug_generation(m_dbg);
    m_runningTicks = 0;
    refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int i = 0; i < 40 && !apple2debug_is_stopped(m_dbg); ++i) QThread::msleep(1);
    resync();
}

/* The engine reports a stop through the generation, so this polls it: a
 * refresh when it moved, and the live values twice a second while the
 * machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = apple2debug_generation(m_dbg);
    const bool stopped = apple2debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped || apple2debug_stop_serial(m_dbg) != m_seenSerial) {
        resync();
    } else if (tabVisible(TAB_WATCHES) && apple2watch_generation() != m_wtGen) {
        refreshWatches(false);          /* the list changed elsewhere (the console) */
    } else if (!stopped && ++m_runningTicks >= 4) {
        m_runningTicks = 0;
        refreshStatus();
        refreshCpu();
        refreshPage(currentTab(), true);
        for (int t = 0; t < TAB_COUNT; ++t)
            if (m_popped[t]) refreshPage(t, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    apple2debug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int n = apple2debug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (n <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int sp = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(sp + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (apple2debug_is_stopped(m_dbg)) {
        apple2debug_resume(m_dbg);
        resync();
    } else {
        apple2debug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(const std::function<void()> &fn)
{
    if (!apple2debug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    fn();
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    stepAnd([this] { apple2debug_run_to(m_dbg, (uint16_t)m_selected); });
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    apple2debug_breakpoint_toggle(m_dbg, (uint16_t)addr);
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* A number typed into an entry: a label first, then bare digits as hex (as in
 * the sibling debuggers), then whatever the engine evaluates ($C000, 0xC000,
 * C000h, a+4, label+2; the console itself reads bare digits as decimal).
 * When it is none of those, the reason goes to *why, or to the status bar
 * without. */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString t = text.trimmed();
    const QByteArray utf8 = t.toUtf8();
    long v = 0;
    QString reason;
    const int label = apple2debug_label_address(m_dbg, utf8.constData());
    if (label >= 0) {
        v = label;
    } else if (hexOnly.match(t).hasMatch()) {
        bool ok;
        v = t.toLong(&ok, 16);
        if (!ok) reason = QStringLiteral("Not a number");
    } else {
        char err[160] = "";
        if (apple2debug_eval(m_dbg, utf8.constData(), &v, err, sizeof err) != 0)
            reason = err[0] ? stripControl(err) : QStringLiteral("Not a number ($C000, or a label)");
    }
    if (reason.isEmpty()) {
        *out = v;
        return true;
    }
    if (why) *why = reason;
    else message(reason);
    return false;
}

/* The same, as an address: 0 to $FFFF. */
bool DebuggerWindow::addressOf(const QString &text, long *out, QString *why)
{
    long v;
    if (!numberOf(text, &v, why)) return false;
    if (v < 0 || v > 0xFFFF) {
        const QString reason = QStringLiteral("Out of range ($0000-$FFFF)");
        if (why) *why = reason;
        else message(reason);
        return false;
    }
    *out = v;
    return true;
}

/* Edits go in only while the machine is stopped. A register entry shows hex
 * digits, so bare digits there are hex; anything else is an expression. */
void DebuggerWindow::setReg(int i)
{
    QLineEdit *e = m_reg[i];
    long v = 0;
    QString why;
    const bool ok = numberOf(e->text(), &v, &why);
    if (!apple2debug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else if (!ok) message(why);
    else {
        /* SP lives in page one; the entry may give just the low byte */
        if (kRegs[i].id == APPLE2_REG_SP) v = 0x100 | (v & 0xFF);
        apple2debug_cpu_set(m_dbg, kRegs[i].id, (int)v);
    }
    e->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(int id, bool on)
{
    if (!apple2debug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else apple2debug_cpu_set(m_dbg, id, on ? 1 : 0);
    resync();
}

/* Shows a range in the dump, from just above it, marked and scrolled to. */
void DebuggerWindow::showInMemory(uint16_t lo, uint16_t hi)
{
    m_hl = true;
    m_hlLo = lo;
    m_hlHi = hi;
    m_memTop = qBound(0, (int)((lo > 0x100 ? lo - 0x100 : 0) & ~0xF), 0x10000 - MEM_ROWS * 16);
    refreshMem();
    const int line = 1 + (lo - m_memTop) / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* Go to: a label, an address, an expression or a range (lo-hi), which is
 * marked. Bare digits are hex, as everywhere in the entries. */
void DebuggerWindow::memGoto()
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString text = m_memGoto->text().trimmed();
    if (text.isEmpty()) return;
    uint16_t lo, hi;
    long a;
    if (hexOnly.match(text).hasMatch() && addressOf(text, &a)) {
        lo = hi = (uint16_t)a;
    } else if (!apple2sym_parse_range(m_dbg, text.toUtf8().constData(), &lo, &hi)) {
        m_hl = false;
        refreshMem();
        message(QStringLiteral("Not an address, label or range: %1").arg(text));
        return;
    }
    showInMemory(lo, hi);
}

/* Find Next / Previous from the marked range (or the top of the dump); the
 * hit is marked. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[APPLE2SEARCH_MAX], mask = 0xFF;
    int n = 0, flags = 0;
    if (!apple2search_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &n, &mask,
                            &flags) || n <= 0) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (A9 00) or some text."));
        return;
    }
    uint16_t from = (uint16_t)m_memTop, found = 0;
    if (m_hl) from = backward ? m_hlLo : (uint16_t)(m_hlLo + 1);
    const int r = apple2debug_search_next(m_dbg, pat, n, mask, flags, from, backward ? 1 : 0, &found);
    if (r < 0) {
        m_srchNote->setText(QStringLiteral("Stop the machine first"));
        message(QStringLiteral("Stop the machine first"));
    } else if (r == 0) {
        m_srchNote->setText(QStringLiteral("Not found"));
        message(QStringLiteral("Not found"));
    } else {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hx(found, 4)));
        showInMemory(found, (uint16_t)(found + n - 1));
    }
}

void DebuggerWindow::memWrite()
{
    long a, v;
    if (!addressOf(m_memAddr->text(), &a)) return;
    if (!numberOf(m_memVal->text(), &v)) return;
    if (v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte ($00-$FF)"));
        return;
    }
    apple2debug_write(m_dbg, (uint16_t)a, (uint8_t)v);
    resync();
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), &a)) return;
    m_followPc->setChecked(false);
    m_selected = (int)a;
    m_disasmTop = (uint16_t)apple2debug_row_address(m_dbg, (uint16_t)a, -(visibleRows() / 3));
    refreshDisasm();
}

/* The type takes an address or label. The end is optional; the condition is
 * an expression the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    static const int types[4] = { APPLE2DEBUG_BP_EXEC, APPLE2DEBUG_BP_READ, APPLE2DEBUG_BP_WRITE,
                                  APPLE2DEBUG_BP_READ | APPLE2DEBUG_BP_WRITE };
    const int sel = qBound(0, m_bpType->currentIndex(), 3);
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    long a, b = 0;
    QString why;
    if (!addressOf(start, &a, &why) || (!end.isEmpty() && !addressOf(end, &b, &why))) {
        m_bpMsg->setText(why);
        return;
    }
    if (end.isEmpty()) b = a;
    if (b < a) {
        m_bpMsg->setText(QStringLiteral("The end is before the start"));
        return;
    }
    char err[256] = "";
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (apple2debug_breakpoint_add(m_dbg, types[sel], (uint16_t)a, (uint16_t)b, cond.constData(), err, sizeof err) < 0) {
        m_bpMsg->setText(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

/* A symbol file the user picks. */
void DebuggerWindow::loadSymbols()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load Symbols"), QString(),
        QStringLiteral("Symbol files (*.sym *.SYM *.sym2 *.SYM2 *.lbl *.map *.txt);;All files (*)"));
    if (path.isEmpty()) return;
    char msg[512] = "";
    apple2debug_load_symbols(m_dbg, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    apple2debug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs in windows of their own ----------------------------------------- */

namespace {
/* A tab's window: closing it docks the tab again. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(e);
    }
};
} // namespace

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;     /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setObjectName(QStringLiteral("debugger-pop-%1").arg(QLatin1String(kTabs[tab].name)));
    win->setWindowTitle(QStringLiteral("Apple II Debugger - %1").arg(QLatin1String(kTabs[tab].label)));
    win->setProperty("apple2HelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].help)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch(1);
    bar->addWidget(apple2HelpButton(QString()));
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    static_cast<PopWindow *>(win)->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    /* the stepping keys work in the window too */
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { apple2HelpFor(win); });
    new QShortcut(QKeySequence(Qt::Key_F5), win, [this] { toggleRun(); });
    new QShortcut(QKeySequence(Qt::Key_F7), win, [this] { stepAnd([this] { apple2debug_step(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F7), win, [this] { stepAnd([this] { apple2debug_frame(m_dbg, 1); }); });
    new QShortcut(QKeySequence(Qt::Key_F8), win, [this] { stepAnd([this] { apple2debug_step_over(m_dbg); }); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), win, [this] { stepAnd([this] { apple2debug_step_out(m_dbg); }); });
    if (isVisible()) win->show();
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int t = 0; t < tab; ++t)       /* back where it was in the order */
        if (!m_popped[t]) ++pos;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    m_tabs->setCurrentIndex(pos);
    if (tab == TAB_HISTORY) m_hSeen[1] = -1;
    refreshPage(tab, false);
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)apple2debug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (e->isAutoRepeat() && (e->key() == Qt::Key_F5 || e->key() == Qt::Key_F9 || e->key() == Qt::Key_F12)) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(shift ? std::function<void()>([this] { apple2debug_frame(m_dbg, 1); })
                                   : std::function<void()>([this] { apple2debug_step(m_dbg); })); break;
    case Qt::Key_F8: stepAnd(shift ? std::function<void()>([this] { apple2debug_step_out(m_dbg); })
                                   : std::function<void()>([this] { apple2debug_step_over(m_dbg); })); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
}

/* Shown: the engine attaches, which stops the machine, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    m_timer.start(100);
    if (e->spontaneous()) return;
    for (QWidget *win : m_popped)
        if (win) win->show();
    apple2debug_attach(m_dbg);
    m_attached = true;
    static bool historyOnce = true;     /* APPLE2_HISTORY: record from the first show */
    if (historyOnce && qEnvironmentVariableIsSet("APPLE2_HISTORY")) apple2debug_trace_enable(m_dbg, 1);
    historyOnce = false;
    m_wasStopped = false;
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        if (m_tabs->currentIndex() == TAB_CONSOLE) m_conIn->setFocus();
        settleAndSync();
    });
}

/* Hidden by F12, the close button or the main window: the engine goes away
 * and the machine runs on. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. A minimised
 * window's spontaneous hide is not a detach. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    if (!e->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        if (m_attached) {
            apple2debug_detach(m_dbg);
            m_attached = false;
        }
    }
    QMainWindow::hideEvent(e);
}
