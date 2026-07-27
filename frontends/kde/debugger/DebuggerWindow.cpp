/*
 * The KDE/Qt frontend's debugger window: run control, 6502 registers,
 * disassembly with click-to-toggle breakpoints, and a memory view.
 *
 * Same shape as the GNOME window, over the same shared engine
 * (core/debugger), which owns the rule that only the emulator thread touches
 * AppleWin. The one thing this file must get right is that the engine's stop
 * callback fires ON THE EMULATOR THREAD, so it is handed to the GUI thread
 * with a queued connection before any widget is touched.
 *
 * Keys match the rest of the family: F5 pause/continue, F7 step into,
 * F8 step over, Shift+F8 step out.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPointer>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

constexpr int kDisasmLines = 24;
/* 8 rather than 16 columns: under a tiling window manager the window gets
 * whatever width the tile is, and a 16-column dump (~70 monospace
 * characters) is wide enough to squeeze the disassembly out of the splitter
 * entirely. 8 columns still shows a useful amount and halves the demand. */
constexpr int kMemCols = 8;
constexpr int kMemRows = 24;

QPointer<DebuggerWindow> g_window;

QFont monoFont(const QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    return f;
}

/* EMULATOR THREAD. Does nothing but hand the event to the GUI thread. */
void stopTrampoline(apple2debug_stop_reason reason, uint16_t pc, void *user)
{
    auto *self = static_cast<DebuggerWindow *>(user);
    QMetaObject::invokeMethod(self, "onStopped", Qt::QueuedConnection,
                              Q_ARG(int, static_cast<int>(reason)),
                              Q_ARG(unsigned, pc));
}

} // namespace

void DebuggerWindow::show(QWidget *parent, apple2session *session)
{
    if (g_window) {
        g_window->raise();
        g_window->activateWindow();
        return;
    }
    auto *w = new DebuggerWindow(session, parent);
    g_window = w;
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->QMainWindow::show();
}

DebuggerWindow::DebuggerWindow(apple2session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session),
      m_dbg(apple2session_debugger(session))
{
    setWindowTitle(QStringLiteral("Debugger"));
    /* Wide enough for both panes at their natural size: the memory view is
     * 16 hex columns plus ASCII (~70 monospace characters) and the
     * disassembly ~45, and if the window is too narrow the splitter squeezes
     * the listing down to nothing. */
    resize(1240, 760);
    buildUi();

    if (!m_dbg)
        return;
    apple2debug_set_stop_callback(m_dbg, stopTrampoline, this);

    /* Open paused on purpose: a debugger that opens on a running machine
     * shows a disassembly that is stale before it is drawn. */
    apple2debug_pause(m_dbg);
    refreshAll();
}

DebuggerWindow::~DebuggerWindow()
{
    if (m_dbg) {
        /* Leave the machine running, but stop calling into a dead window. */
        apple2debug_set_stop_callback(m_dbg, nullptr, nullptr);
        apple2debug_resume(m_dbg);
    }
}

void DebuggerWindow::buildUi()
{
    auto *tb = addToolBar(QStringLiteral("Run"));
    tb->setMovable(false);

    m_run = new QPushButton(QStringLiteral("Pause"), this);
    m_run->setToolTip(QStringLiteral("Pause / Continue (F5)"));
    connect(m_run, &QPushButton::clicked, this, &DebuggerWindow::toggleRun);
    tb->addWidget(m_run);

    auto addStep = [this, tb](const QString &label, const QString &tip,
                              void (*fn)(apple2debug *)) {
        auto *b = new QPushButton(label, this);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, fn] {
            if (m_dbg) fn(m_dbg);
        });
        tb->addWidget(b);
    };
    addStep(QStringLiteral("Into"), QStringLiteral("Step into (F7)"),
            apple2debug_step_into);
    addStep(QStringLiteral("Over"), QStringLiteral("Step over (F8)"),
            apple2debug_step_over);
    addStep(QStringLiteral("Out"), QStringLiteral("Step out (Shift+F8)"),
            apple2debug_step_out);

    m_status = new QLabel(QStringLiteral("Running"), this);
    statusBar()->addPermanentWidget(m_status);

    /* Disassembly on the left, registers and memory on the right. */
    m_disasm = new QListWidget(this);
    m_disasm->setFont(monoFont(this));
    m_disasm->setToolTip(
        QStringLiteral("Click a line to toggle a breakpoint"));
    /* itemClicked, not itemActivated: activation needs a double click by
     * default, and a single click is what anyone expects from a breakpoint
     * gutter. Connecting both would toggle twice on a double click. */
    connect(m_disasm, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                if (!m_dbg || !item) return;
                const quint16 addr =
                    static_cast<quint16>(item->data(Qt::UserRole).toUInt());
                apple2debug_bp_toggle(m_dbg, addr);
                renderDisasm();
            });

    m_regs = new QLabel(this);
    m_regs->setFont(monoFont(this));
    m_regs->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_regs->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_memAddr = new QLineEdit(this);
    m_memAddr->setPlaceholderText(QStringLiteral("Address or symbol"));
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] {
        if (!m_dbg) return;
        QString text = m_memAddr->text().trimmed();
        while (text.startsWith(QLatin1Char('$'))) text.remove(0, 1);
        uint16_t sym = 0;
        bool ok = false;
        if (apple2debug_symbol_find(m_dbg, text.toUtf8().constData(), &sym)) {
            m_memAt = sym;
            ok = true;
        } else {
            const uint dec = text.toUInt(&ok, 16);
            if (ok) m_memAt = static_cast<quint16>(dec);
        }
        if (ok) renderMem();
    });

    m_mem = new QLabel(this);
    m_mem->setFont(monoFont(this));
    m_mem->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_mem->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *regBox = new QGroupBox(QStringLiteral("Registers"), this);
    auto *regLayout = new QVBoxLayout(regBox);
    regLayout->addWidget(m_regs);

    auto *memBox = new QGroupBox(QStringLiteral("Memory"), this);
    auto *memLayout = new QVBoxLayout(memBox);
    memLayout->addWidget(m_memAddr);
    memLayout->addWidget(m_mem);
    memLayout->addStretch();

    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(regBox);
    rightLayout->addWidget(memBox, 1);

    /* Scrolled, so the right pane's natural width never becomes a hard
     * minimum the splitter has to honour at the disassembly's expense.
     * The scroll area still propagates its child's minimum, so say outright
     * that the listing gets the space: under a tiling window manager the
     * window is whatever width the tile is, and without this the register
     * and memory panes crowd the disassembly out entirely. */
    auto *rightScroll = new QScrollArea(this);
    rightScroll->setWidget(right);
    rightScroll->setWidgetResizable(true);
    rightScroll->setFrameShape(QFrame::NoFrame);
    rightScroll->setMinimumWidth(220);
    m_disasm->setMinimumWidth(430);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(m_disasm);
    split->addWidget(rightScroll);
    split->setSizes({560, 680});
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    setCentralWidget(split);

    /* Shortcuts rather than keyPressEvent: they fire wherever focus is,
     * including inside the disassembly list. */
    connect(new QShortcut(QKeySequence(Qt::Key_F5), this),
            &QShortcut::activated, this, &DebuggerWindow::toggleRun);
    connect(new QShortcut(QKeySequence(Qt::Key_F7), this),
            &QShortcut::activated, this,
            [this] { if (m_dbg) apple2debug_step_into(m_dbg); });
    connect(new QShortcut(QKeySequence(Qt::Key_F8), this),
            &QShortcut::activated, this,
            [this] { if (m_dbg) apple2debug_step_over(m_dbg); });
    connect(new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), this),
            &QShortcut::activated, this,
            [this] { if (m_dbg) apple2debug_step_out(m_dbg); });
}

void DebuggerWindow::renderRegs()
{
    if (!m_dbg) return;
    apple2debug_regs r;
    apple2debug_get_regs(m_dbg, &r);

    /* Bit 7 is N, bit 0 is C; a set flag shows in upper case and a clear one
     * as a dot, the way every 6502 monitor has always done it. */
    static const char kFlag[] = "NV-BDIZC";
    QString flags;
    for (int i = 0; i < 8; i++)
        flags += (r.ps & (0x80 >> i)) ? QChar(kFlag[i]) : QChar('.');

    /* Format each field on its own: uppercasing the whole assembled string
     * would also reach the parts that are case-significant. */
    auto hex = [](uint v, int w) {
        return QStringLiteral("%1").arg(v, w, 16, QLatin1Char('0')).toUpper();
    };

    /* SP is a full address because that is what AppleWin stores: the 6502's
     * 8-bit stack pointer plus the $0100 page base. */
    QString text = QStringLiteral("PC  $%1\nA   $%2      X   $%3\n"
                                  "Y   $%4      SP  $%5\nP   $%6   %7")
                       .arg(hex(r.pc, 4), hex(r.a, 2), hex(r.x, 2),
                            hex(r.y, 2), hex(r.sp, 4), hex(r.ps, 2), flags);
    if (r.jammed)
        text += QStringLiteral("\n\nCPU JAMMED");
    m_regs->setText(text);
}

void DebuggerWindow::renderDisasm()
{
    if (!m_dbg) return;
    apple2dasm_line lines[kDisasmLines];
    apple2debug_regs r;
    uint16_t next = 0;

    apple2debug_get_regs(m_dbg, &r);
    if (m_followPc) {
        m_disasmAt = r.pc;
        m_followPc = false;
    }

    const int n =
        apple2debug_disassemble(m_dbg, m_disasmAt, kDisasmLines, lines, &next);

    m_disasm->clear();
    for (int i = 0; i < n; i++) {
        QString bytes;
        for (int b = 0; b < lines[i].len; b++)
            bytes += QStringLiteral("%1 ")
                         .arg(lines[i].bytes[b], 2, 16, QLatin1Char('0'))
                         .toUpper();

        const QString addr =
            QStringLiteral("%1").arg(lines[i].addr, 4, 16, QLatin1Char('0'))
                .toUpper();
        const QString sym =
            lines[i].symbol
                ? QStringLiteral("  ; %1").arg(QString::fromUtf8(lines[i].symbol))
                : QString();

        const QString text =
            QStringLiteral("%1%2 $%3  %4 %5%6")
                .arg(apple2debug_bp_is_set(m_dbg, lines[i].addr)
                         ? QStringLiteral("*") : QStringLiteral(" "),
                     lines[i].addr == r.pc ? QStringLiteral(">")
                                           : QStringLiteral(" "),
                     addr,
                     bytes.leftJustified(9),
                     QString::fromUtf8(lines[i].text).leftJustified(16),
                     sym);

        auto *item = new QListWidgetItem(text, m_disasm);
        item->setData(Qt::UserRole, static_cast<uint>(lines[i].addr));
    }
}

void DebuggerWindow::renderMem()
{
    if (!m_dbg) return;
    QString out;
    uint8_t row[kMemCols];

    for (int r = 0; r < kMemRows; r++) {
        const quint16 addr = static_cast<quint16>(m_memAt + r * kMemCols);
        apple2debug_read_mem(m_dbg, addr, row, kMemCols);
        out += QStringLiteral("$%1 ")
                   .arg(QStringLiteral("%1")
                            .arg(addr, 4, 16, QLatin1Char('0')).toUpper());
        for (int c = 0; c < kMemCols; c++)
            out += QStringLiteral("%1 ")
                       .arg(QStringLiteral("%1")
                                .arg(row[c], 2, 16, QLatin1Char('0')).toUpper());
        out += QLatin1Char(' ');
        for (int c = 0; c < kMemCols; c++) {
            /* Apple II text carries the high bit; mask it before deciding
             * whether a byte is printable, or nothing ever looks like text. */
            const uint8_t ch = row[c] & 0x7F;
            out += (ch >= 0x20 && ch < 0x7F) ? QChar(ch) : QChar('.');
        }
        out += QLatin1Char('\n');
    }
    m_mem->setText(out);
}

void DebuggerWindow::refreshAll()
{
    if (!m_dbg) return;
    const bool paused = apple2debug_is_paused(m_dbg);
    m_run->setText(paused ? QStringLiteral("Continue")
                          : QStringLiteral("Pause"));
    renderRegs();
    renderDisasm();
    renderMem();
}

void DebuggerWindow::toggleRun()
{
    if (!m_dbg) return;
    if (apple2debug_is_paused(m_dbg)) {
        apple2debug_resume(m_dbg);
        m_status->setText(QStringLiteral("Running"));
        refreshAll();
    } else {
        apple2debug_pause(m_dbg);
    }
}

void DebuggerWindow::onStopped(int reason, unsigned pc)
{
    static const char *const kReason[] = {
        "Paused", "Breakpoint", "Stepped", "Ran to address", "CPU JAMMED"
    };
    const char *what = (reason >= 0 && reason <= APPLE2DBG_STOP_JAMMED)
                           ? kReason[reason] : "Stopped";
    m_status->setText(
        QStringLiteral("%1 at $%2")
            .arg(QLatin1String(what),
                 QStringLiteral("%1").arg(pc, 4, 16, QLatin1Char('0'))
                     .toUpper()));
    m_followPc = true;
    refreshAll();
}
