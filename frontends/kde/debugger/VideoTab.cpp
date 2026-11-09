/*
 * The debugger's Video tab (Qt6 Widgets).
 *
 * Everything that decides what to show lives in the core (apple2vidtab.h): this file is widgets.
 * The picture comes from the controller's cached buffer already opaque (0xFFRRGGBB), so
 * Format_RGB32 is safe on KWin; a mouse move redraws only the marks, never re-renders.
 * Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "VideoTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

const QColor kAccent(0xf4, 0x43, 0x36);

QLabel *monoLabel()
{
    auto *l = new QLabel;
    QFont f = l->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    l->setFont(f);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return l;
}

QTableWidget *makeTable(const QStringList &heads)
{
    auto *t = new QTableWidget(0, heads.size());
    t->setHorizontalHeaderLabels(heads);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->horizontalHeader()->setStretchLastSection(true);
    QFont f = t->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    t->setFont(f);
    return t;
}

QString hex(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

/* hot: drawn in the accent; dim: grey ink */
void setCell(QTableWidget *t, int row, int col, const QString &text, bool hot, bool dim = false)
{
    auto *it = t->item(row, col);
    if (!it) {
        it = new QTableWidgetItem;
        t->setItem(row, col, it);
    }
    it->setText(text);
    it->setBackground(hot ? QBrush(kAccent) : QBrush());
    it->setForeground(hot ? QBrush(Qt::white) : dim ? QBrush(Qt::gray) : QBrush());
}

} // namespace

/* ---- the picture ------------------------------------------------------------ */

class VideoPicture : public QWidget
{
public:
    explicit VideoPicture(VideoTab *owner) : m_owner(owner)
    {
        setMouseTracking(true);
        setMinimumSize(320, 200);
    }
    /* the controller's buffer, or a message when this view cannot be drawn */
    void sync()
    {
        int w = 0, h = 0;
        const uint32_t *px = apple2vid_tab_picture(m_owner->m_tab, &w, &h);
        if (px) {
            /* a view over the controller's buffer: valid until its next refresh. Nearest-neighbour
             * is the controller's own scaling; the widget draws it 1:1 */
            m_image = QImage(reinterpret_cast<const uchar *>(px), w, h, w * 4, QImage::Format_RGB32);
            m_msg.clear();
            setFixedSize(w, h);
        } else {
            m_image = QImage();
            const char *why = apple2vid_tab_why(m_owner->m_tab);
            m_msg = QString::fromUtf8(why ? why : "Nothing to show");
            setMinimumSize(320, 200);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(18, 18, 24));
        if (m_image.isNull()) {
            p.setPen(QColor(140, 140, 148));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_msg);
            return;
        }
        p.drawImage(0, 0, m_image);
        int hover[4] = { 0 }, pin[4] = { 0 };
        const int marks = apple2vid_tab_marks(m_owner->m_tab, hover, pin);
        if (marks & 1) {
            p.setPen(QPen(QColor(255, 255, 255, 230), 1));
            p.drawRect(hover[0], hover[1], hover[2] - 1, hover[3] - 1);
        }
        if (marks & 2) {
            p.setPen(QPen(QColor(255, 217, 26), 2));
            p.drawRect(pin[0], pin[1], pin[2] - 1, pin[3] - 1);
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const QPoint pt = e->position().toPoint();
        if (apple2vid_tab_hover(m_owner->m_tab, pt.x(), pt.y())) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void leaveEvent(QEvent *) override
    {
        if (apple2vid_tab_hover(m_owner->m_tab, -1, -1)) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pt = e->position().toPoint();
        apple2vid_tab_pin(m_owner->m_tab, pt.x(), pt.y());
        update();
        m_owner->updatePixelSection();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pt = e->position().toPoint();
        apple2vid_tab_hover(m_owner->m_tab, pt.x(), pt.y());
        m_owner->breakOnPixel();
        update();
    }

private:
    VideoTab *m_owner;
    QImage m_image;
    QString m_msg;
};

/* ---- the tab ------------------------------------------------------------------ */

VideoTab::VideoTab(apple2session *session, QWidget *parent) : QWidget(parent)
{
    m_tab = apple2vid_tab_new(session);
    m_dbg = session ? apple2session_debugger(session) : nullptr;

    auto *root = new QVBoxLayout(this);

    /* ---- toolbar: view, follow, mode, page, mixed, zoom */
    auto *bar = new QHBoxLayout;
    m_view = new QComboBox;
    m_view->setObjectName(QStringLiteral("videoView"));
    for (int i = 0; i < APPLE2VID_V_COUNT; ++i) m_view->addItem(QString::fromUtf8(apple2vid_view_name(i)));
    m_view->setToolTip(QStringLiteral("Screen: the last finished frame, as the monitor showed it.\n"
                                      "Decoded: video memory through the soft switches (or the mode chosen here)."));
    m_follow = new QCheckBox(QStringLiteral("Follow screen"));
    m_follow->setObjectName(QStringLiteral("videoFollow"));
    m_follow->setToolTip(QStringLiteral("Decoded tracks the displayed mode, page and Mixed; choosing one yourself turns this off"));
    m_mode = new QComboBox;
    m_mode->setObjectName(QStringLiteral("videoMode"));
    for (int i = 0; i < APPLE2VID_M_COUNT; ++i) m_mode->addItem(QString::fromUtf8(apple2vid_mode_name(i)));
    m_page = new QComboBox;
    m_page->setObjectName(QStringLiteral("videoPage"));
    m_page->addItems({ QStringLiteral("Page 1"), QStringLiteral("Page 2") });
    m_mixed = new QCheckBox(QStringLiteral("Mixed"));
    m_mixed->setObjectName(QStringLiteral("videoMixed"));
    m_mixed->setToolTip(QStringLiteral("Four lines of text under the graphics"));
    m_zoom = new QComboBox;
    m_zoom->setObjectName(QStringLiteral("videoZoom"));
    int nz = 0;
    const int *zooms = apple2vid_zooms(&nz);
    for (int i = 0; i < nz; ++i) m_zoom->addItem(zooms[i] ? QStringLiteral("%1×").arg(zooms[i]) : QStringLiteral("Fit"), zooms[i]);
    bar->addWidget(new QLabel(QStringLiteral("View")));
    bar->addWidget(m_view);
    bar->addWidget(m_follow);
    bar->addWidget(m_mode);
    bar->addWidget(m_page);
    bar->addWidget(m_mixed);
    bar->addWidget(new QLabel(QStringLiteral("Zoom")));
    bar->addWidget(m_zoom);
    bar->addStretch(1);
    root->addLayout(bar);

    /* ---- overlays */
    auto *ovRow = new QHBoxLayout;
    ovRow->addWidget(new QLabel(QStringLiteral("Overlays")));
    int n = 0;
    const apple2vid_overlay_info *od = apple2vid_overlays(&n);
    for (int i = 0; i < n; ++i) {
        auto *c = new QCheckBox(QString::fromUtf8(od[i].label));
        c->setObjectName(QStringLiteral("videoOverlay_%1").arg(QString::fromUtf8(od[i].name)));
        c->setProperty("bit", od[i].bit);
        m_ov << c;
        ovRow->addWidget(c);
    }
    ovRow->addStretch(1);
    root->addLayout(ovRow);

    m_status = monoLabel();
    root->addWidget(m_status);

    auto *split = new QSplitter(Qt::Horizontal);
    root->addWidget(split, 1);

    /* ---- left: picture */
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea;
    m_pic = new VideoPicture(this);
    m_pic->setObjectName(QStringLiteral("videoPicture"));
    m_scroll->setWidget(m_pic);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    lv->addWidget(m_scroll, 1);
    m_note = new QLabel;
    m_note->setObjectName(QStringLiteral("videoNote"));
    lv->addWidget(m_note);
    split->addWidget(left);

    /* ---- right: sections, in apple2vid_section_name order */
    m_sections = new QTabWidget;
    m_sections->setObjectName(QStringLiteral("videoSections"));
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_pixelText = monoLabel();
        m_pixelText->setObjectName(QStringLiteral("videoPixel"));
        m_pixelText->setWordWrap(true);
        v->addWidget(m_pixelText);
        auto *btns = new QHBoxLayout;
        auto *brk = new QPushButton(QStringLiteral("Break on writes"));
        brk->setToolTip(QStringLiteral("Write breakpoints on the inspected pixel's byte (main, and aux when the mode uses it)"));
        auto *mem = new QPushButton(QStringLiteral("Show in Memory"));
        mem->setToolTip(QStringLiteral("Show the inspected pixel's byte in the Memory tab (the aux bank when only aux applies)"));
        auto *unpin = new QPushButton(QStringLiteral("Unpin"));
        btns->addWidget(brk);
        btns->addWidget(mem);
        btns->addWidget(unpin);
        btns->addStretch(1);
        v->addLayout(btns);
        v->addStretch(1);
        connect(brk, &QPushButton::clicked, this, [this] { breakOnPixel(); });
        connect(mem, &QPushButton::clicked, this, [this] { showPixelInMemory(); });
        connect(unpin, &QPushButton::clicked, this, [this] {
            apple2vid_tab_unpin(m_tab);
            m_pic->update();
            updatePixelSection();
        });
        m_sections->addTab(w, QString::fromUtf8(apple2vid_section_name(APPLE2VID_S_PIXEL)));
    }
    m_switches = makeTable({ QStringLiteral("Switch"), QStringLiteral("State"), QStringLiteral("Age"),
                             QStringLiteral("Set"), QStringLiteral("Clear"), QStringLiteral("Meaning"), QStringLiteral("") });
    m_switches->setObjectName(QStringLiteral("videoSwitches"));
    m_switches->setRowCount(APPLE2VID_SW_COUNT);
    m_switches->verticalHeader()->setDefaultSectionSize(28);
    for (int i = 0; i < APPLE2VID_SW_COUNT; ++i) {
        auto *b = new QPushButton(QStringLiteral("Break on change"));
        b->setToolTip(QStringLiteral("Read and write breakpoints on both of the switch's addresses"));
        connect(b, &QPushButton::clicked, this, [this, i] {
            const int id = apple2vid_tab_break_on_switch(m_tab, i);
            const apple2vid_switch *sw = nullptr;
            const int cnt = apple2vid_tab_switches(m_tab, &sw);
            if (id >= 0 && i < cnt)
                note(QStringLiteral("Breakpoints added on %1: $%2 and $%3 (see Breakpoints)")
                         .arg(QString::fromUtf8(sw[i].name), hex(sw[i].set_addr, 4), hex(sw[i].clr_addr, 4)));
            else
                note(QStringLiteral("No breakpoint added"));
        });
        m_switches->setCellWidget(i, 6, b);
    }
    m_sections->addTab(m_switches, QString::fromUtf8(apple2vid_section_name(APPLE2VID_S_SWITCHES)));
    m_colours = makeTable({ QStringLiteral(""), QStringLiteral("Colour"), QStringLiteral("RGB"), QStringLiteral("Pixels"),
                            QStringLiteral("") });
    m_colours->setObjectName(QStringLiteral("videoColours"));
    m_sections->addTab(m_colours, QString::fromUtf8(apple2vid_section_name(APPLE2VID_S_COLOURS)));
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_beam = new QProgressBar;
        m_beam->setRange(0, 1000);
        m_beam->setFormat(QStringLiteral("beam"));
        v->addWidget(m_beam);
        m_timingText = monoLabel();
        m_timingText->setObjectName(QStringLiteral("videoTiming"));
        v->addWidget(m_timingText);
        v->addStretch(1);
        m_sections->addTab(w, QString::fromUtf8(apple2vid_section_name(APPLE2VID_S_TIMING)));
    }
    split->addWidget(m_sections);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    m_sections->setMinimumWidth(420);
    split->setSizes({ 700, 450 });

    /* start from the controller's state (the APPLE2_DEBUGGER_VIDEO_* variables) */
    m_updating = true;
    m_view->setCurrentIndex(apple2vid_tab_view(m_tab));
    for (int i = 0; i < nz; ++i)
        if (zooms[i] == apple2vid_tab_zoom(m_tab)) m_zoom->setCurrentIndex(i);
    m_sections->setCurrentIndex(apple2vid_tab_env_section(m_tab));
    m_updating = false;
    syncControls();

    connect(m_view, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        apple2vid_tab_set_view(m_tab, i);
        refresh(true);
    });
    connect(m_follow, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating) return;
        apple2vid_tab_set_follow(m_tab, on ? 1 : 0);
        refresh(true);
    });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        apple2vid_tab_set_mode(m_tab, i);
        refresh(true);
    });
    connect(m_page, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        apple2vid_tab_set_page(m_tab, i + 1);
        refresh(true);
    });
    connect(m_mixed, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating) return;
        apple2vid_tab_set_mixed(m_tab, on ? 1 : 0);
        refresh(true);
    });
    connect(m_zoom, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0) return;
        apple2vid_tab_set_zoom(m_tab, m_zoom->itemData(i).toInt());
        refresh(true);
    });
    for (QCheckBox *c : m_ov)
        connect(c, &QCheckBox::toggled, this, [this](bool) {
            if (m_updating) return;
            unsigned m = 0;
            for (QCheckBox *k : m_ov)
                if (k->isChecked()) m |= k->property("bit").toUInt();
            apple2vid_tab_set_overlays(m_tab, m);
            refresh(true);
        });
    connect(m_sections, &QTabWidget::currentChanged, this, [this](int) { refreshView(APPLE2VID_TAB_TABLES); });
}

VideoTab::~VideoTab()
{
    apple2vid_tab_free(m_tab);
}

void VideoTab::note(const QString &text)
{
    m_note->setText(text);
}

/* The controls show the controller's state: its getters follow the machine while Follow is on. */
void VideoTab::syncControls()
{
    m_updating = true;
    const bool decoded = apple2vid_tab_view(m_tab) == APPLE2VID_V_DECODED;
    m_view->setCurrentIndex(apple2vid_tab_view(m_tab));
    m_follow->setChecked(apple2vid_tab_follow(m_tab) != 0);
    m_mode->setCurrentIndex(apple2vid_tab_mode(m_tab));
    m_page->setCurrentIndex(apple2vid_tab_page(m_tab) == 2 ? 1 : 0);
    m_mixed->setChecked(apple2vid_tab_mixed(m_tab) != 0);
    for (QWidget *w : { static_cast<QWidget *>(m_follow), static_cast<QWidget *>(m_mode),
                        static_cast<QWidget *>(m_page), static_cast<QWidget *>(m_mixed) })
        w->setEnabled(decoded);
    const unsigned ov = apple2vid_tab_overlays(m_tab);
    for (QCheckBox *c : m_ov) c->setChecked((ov & c->property("bit").toUInt()) != 0);
    const int z = apple2vid_tab_zoom(m_tab);
    for (int i = 0; i < m_zoom->count(); ++i)
        if (m_zoom->itemData(i).toInt() == z) m_zoom->setCurrentIndex(i);
    m_updating = false;
}

void VideoTab::showDecoded(int mode, int page)
{
    apple2vid_tab_set_view(m_tab, APPLE2VID_V_DECODED);
    apple2vid_tab_set_mode(m_tab, mode);
    apple2vid_tab_set_page(m_tab, page);
    refresh(true);
}

/* Taken when the tab is first on screen, so the zoom (Fit) has its room. */
void VideoTab::pinAt(int x, int y)
{
    m_pendingPin = QPoint(x, y);
    m_havePin = true;
    if (isVisible()) refresh(true);
}

void VideoTab::breakOnPixel()
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(m_tab);
    const int main = p && p->valid ? p->main_addr : -1, aux = p && p->valid ? p->aux_addr : -1;
    /* a second double-click on the same byte adds nothing */
    auto have = [this](int addr) {
        if (addr < 0 || !m_dbg) return addr < 0;
        static apple2debug_breakpoint bps[256];
        const int n = apple2debug_breakpoint_list(m_dbg, bps, 256);
        for (int i = 0; i < n; ++i)
            if (bps[i].enabled && (bps[i].type & APPLE2DEBUG_BP_WRITE) && bps[i].start == addr && bps[i].end == addr)
                return true;
        return false;
    };
    const bool already = main >= 0 && have(main) && have(aux);
    const int id = already ? 0 : apple2vid_tab_break_on_pixel(m_tab);
    if (id >= 0 && (main >= 0 || aux >= 0)) {
        QString what;
        if (main >= 0) what += QStringLiteral("$") + hex(main, 4);
        if (aux >= 0) what += QStringLiteral("%1aux $%2").arg(main >= 0 ? QStringLiteral(" and ") : QString(), hex(aux, 4));
        note(QStringLiteral("Break on writes to %1 (see Breakpoints)").arg(what));
    } else {
        note(QStringLiteral("Nothing to break on: hover or pin a pixel inside the picture"));
    }
    updatePixelSection();
}

void VideoTab::showPixelInMemory()
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(m_tab);
    if (!p || !p->valid) {
        note(QStringLiteral("Hover or pin a pixel first"));
        return;
    }
    if (p->main_addr >= 0)
        emit showInMemory(APPLE2_BANK_MAIN, p->main_addr, p->main_addr);
    else if (p->aux_addr >= 0)
        emit showInMemory(APPLE2_BANK_AUX, p->aux_addr, p->aux_addr);
    else
        note(QStringLiteral("That pixel has no memory byte"));
}

void VideoTab::updatePixelSection()
{
    const apple2vid_pixel *p = apple2vid_tab_pixel(m_tab);
    m_pixelText->setText(p ? QString::fromUtf8(p->text)
                           : QStringLiteral("Move over the picture to inspect a pixel.\nClick to pin it; "
                                            "double-click to break on writes to it."));
    if (m_sections->currentIndex() == APPLE2VID_S_COLOURS) fillColours();
}

void VideoTab::fillSwitches()
{
    const apple2vid_switch *s;
    const int n = apple2vid_tab_switches(m_tab, &s);
    for (int i = 0; i < n && i < APPLE2VID_SW_COUNT; ++i) {
        const bool hot = s[i].age < APPLE2VID_SW_FADE;
        const bool dim = !hot;
        setCell(m_switches, i, 0, QString::fromUtf8(s[i].name), hot);
        setCell(m_switches, i, 1, s[i].on ? QStringLiteral("on") : QStringLiteral("off"), hot);
        setCell(m_switches, i, 2, QString::number(s[i].age), hot, dim);
        setCell(m_switches, i, 3, QStringLiteral("$") + hex(s[i].set_addr, 4), hot);
        setCell(m_switches, i, 4, QStringLiteral("$") + hex(s[i].clr_addr, 4), hot);
        setCell(m_switches, i, 5, QString::fromUtf8(s[i].meaning), hot);
    }
    m_switches->resizeColumnsToContents();
}

void VideoTab::fillColours()
{
    const apple2vid_pal_entry *e;
    const int n = apple2vid_tab_palette(m_tab, &e);
    m_colours->setRowCount(n);
    /* the controller marks the entry at its next refresh; hover and pin move faster */
    const int inspectedNow = apple2vid_tab_pixel_palette_index(m_tab);
    for (int i = 0; i < n; ++i) {
        const bool mark = e[i].inspected || i == inspectedNow;
        auto *sw = m_colours->item(i, 0);
        if (!sw) {
            sw = new QTableWidgetItem;
            m_colours->setItem(i, 0, sw);
        }
        sw->setBackground(QBrush(QColor::fromRgb(e[i].rgb & 0xFFFFFF)));
        setCell(m_colours, i, 1, QString::fromUtf8(e[i].name), false);
        setCell(m_colours, i, 2, QStringLiteral("#") + hex(e[i].rgb & 0xFFFFFF, 6), false);
        setCell(m_colours, i, 3, QStringLiteral("%1 pixels").arg(e[i].uses), false);
        setCell(m_colours, i, 4, mark ? QStringLiteral("◀ inspected") : QString(), mark);
    }
    m_colours->resizeColumnsToContents();
    m_colours->setColumnWidth(0, 28);
}

void VideoTab::fillTiming()
{
    const char *const *lines;
    const int n = apple2vid_tab_timing(m_tab, &lines);
    QString s;
    for (int i = 0; i < n; ++i) {
        const QString line = QString::fromUtf8(lines[i]);
        const int t = line.indexOf(QLatin1Char('\t'));
        s += t >= 0 ? line.left(t).leftJustified(14) + QLatin1Char(' ') + line.mid(t + 1) : line;
        s += QLatin1Char('\n');
    }
    m_timingText->setText(s.isEmpty() ? QStringLiteral("No timing yet") : s);
    m_beam->setValue(apple2vid_tab_beam_permille(m_tab));
}

void VideoTab::refreshView(unsigned what)
{
    if (what & APPLE2VID_TAB_PICTURE) m_pic->sync();
    if (what & APPLE2VID_TAB_TABLES) {
        m_status->setText(QString::fromUtf8(apple2vid_tab_status(m_tab)));
        syncControls();
        switch (m_sections->currentIndex()) {
        case APPLE2VID_S_SWITCHES: fillSwitches(); break;
        case APPLE2VID_S_COLOURS: fillColours(); break;
        case APPLE2VID_S_TIMING: fillTiming(); break;
        default: break;
        }
        updatePixelSection();
    }
}

void VideoTab::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    /* Fit follows the room the picture has */
    if (apple2vid_tab_zoom(m_tab) == 0) refresh(false);
}

void VideoTab::refresh(bool force)
{
    const QSize vp = m_scroll->viewport()->size();
    if (vp.width() > 4 && vp.height() > 4) apple2vid_tab_set_viewport(m_tab, vp.width() - 2, vp.height() - 2);
    unsigned what = apple2vid_tab_refresh(m_tab, force);
    if (force) what |= APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;
    if (what & APPLE2VID_TAB_OFF) what |= APPLE2VID_TAB_PICTURE | APPLE2VID_TAB_TABLES;
    refreshView(what);
    if (m_havePin && isVisible() && vp.width() > 4 && !(what & APPLE2VID_TAB_OFF)) {
        m_havePin = false;
        apple2vid_tab_pin(m_tab, m_pendingPin.x(), m_pendingPin.y());
        m_pic->update();
        updatePixelSection();
    }
}
