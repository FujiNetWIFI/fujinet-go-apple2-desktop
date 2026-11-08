/*
 * HelpWindow -- see HelpWindow.h.
 *
 * The topics on the left, with a search that lists every section having
 * all the words typed; the topic on the right, as HTML built from its
 * blocks, its matches highlighted when it came from a search.
 *
 * Preferences and the message boxes are application-modal (exec), which
 * blocks every other window: while one is open the Help belongs to it, and
 * goes back to the main window when it closes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "HelpWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "apple2help.h"

namespace {

constexpr unsigned ME = APPLE2HELP_KDE;
constexpr int MAX_HITS = 64;
constexpr int TOPIC_ROLE = Qt::UserRole, BLOCK_ROLE = Qt::UserRole + 1;

QString utf8(const char *s) { return QString::fromUtf8(s ? s : ""); }

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t),
                            float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

QString spanHtml(const apple2help_span &sp)
{
    const QString t = utf8(apple2help_span_text(&sp, ME)).toHtmlEscaped();
    switch (sp.kind) {
    case APPLE2HELP_BOLD: return QStringLiteral("<b>%1</b>").arg(t);
    case APPLE2HELP_ITALIC: return QStringLiteral("<i>%1</i>").arg(t);
    case APPLE2HELP_CODE: return QStringLiteral("<code>&#8201;%1&#8201;</code>").arg(t);
    case APPLE2HELP_KBD: return QStringLiteral("<code><b>&#8201;%1&#8201;</b></code>").arg(t);
    case APPLE2HELP_LINK:
        return QStringLiteral("<a href=\"%1\">%2</a>").arg(utf8(sp.target).toHtmlEscaped(), t);
    default: return t;
    }
}

/* spans [from, to) */
QString spansHtml(const apple2help_block &b, int from, int to)
{
    QString out;
    for (int i = from; i < to; i++)
        out += spanHtml(b.spans[i]);
    return out;
}

class HelpWindow : public QWidget {
public:
    explicit HelpWindow(QWidget *home);
    void showAt(const QString &id);
    void findFocus();
    /* Belongs to the modal dialog that is open (but not leaving), else to
     * the main window. */
    void follow(QWidget *leaving = nullptr);

    bool open = false;          /* shown, and not closed since */

protected:
    void changeEvent(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void fillList();
    void showTopic(int topic, int block, const QString &query);
    void render();
    QString html() const;
    void highlight();
    void scrollToBlock();

    QPointer<QWidget> m_home;
    QLineEdit *m_search;
    QListWidget *m_list;
    QTextBrowser *m_view;
    int m_topic = -1, m_block = -1;
    QString m_query;
    bool m_filling = false;
};

QPointer<HelpWindow> g_help;

HelpWindow::HelpWindow(QWidget *home) : QWidget(home, Qt::Window), m_home(home)
{
    setObjectName(QStringLiteral("help"));
    setWindowTitle(QStringLiteral("FujiNet Go Apple II Help"));
    resize(980, 720);

    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search Help"));
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);
    m_list = new QListWidget;
    m_list->setFrameShape(QFrame::NoFrame);
    m_view = new QTextBrowser;
    m_view->setOpenLinks(false);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->document()->setDocumentMargin(24);

    auto *side = new QWidget;
    auto *sv = new QVBoxLayout(side);
    sv->setContentsMargins(6, 6, 0, 6);
    sv->addWidget(m_search);
    sv->addWidget(m_list, 1);
    auto *split = new QSplitter;
    split->addWidget(side);
    split->addWidget(m_view);
    split->setStretchFactor(1, 1);
    split->setSizes({ 260, 720 });
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(split);

    connect(m_search, &QLineEdit::textChanged, this, [this] { fillList(); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (m_list->count() && (m_list->item(0)->flags() & Qt::ItemIsEnabled))
            m_list->setCurrentRow(0);
    });
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (m_filling || !it || !(it->flags() & Qt::ItemIsEnabled))
            return;
        showTopic(it->data(TOPIC_ROLE).toInt(), it->data(BLOCK_ROLE).toInt(), m_search->text().trimmed());
    });
    connect(m_view, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        const QString s = url.toString();
        if (s.startsWith(QLatin1Char('#')))
            showAt(s);
        else
            QDesktopServices::openUrl(url);
    });
    new QShortcut(QKeySequence::Find, this, [this] { findFocus(); });
    fillList();
}

void HelpWindow::findFocus()
{
    m_search->setFocus();
    m_search->selectAll();
}

void HelpWindow::follow(QWidget *leaving)
{
    QWidget *modal = QApplication::activeModalWidget();
    if (modal == this || modal == leaving)
        modal = nullptr;
    QWidget *want = modal ? modal : m_home.data();
    if (parentWidget() == want)
        return;
    /* hidden with the dialog: shown again, once it has gone, if it was open */
    const QRect geo = geometry();
    setParent(want, Qt::Window);
    if (geo.isValid())
        setGeometry(geo);
    if (open)
        QTimer::singleShot(0, this, [this] { show(); });
    if (auto *dlg = qobject_cast<QDialog *>(modal))
        connect(dlg, &QDialog::finished, this, [this, dlg] { follow(dlg); }, Qt::SingleShotConnection);
}

void HelpWindow::showAt(const QString &id)
{
    if (id.startsWith(QLatin1Char('?'))) {
        m_search->setText(id.mid(1));
        if (m_list->count() && (m_list->item(0)->flags() & Qt::ItemIsEnabled))
            m_list->setCurrentRow(0);
        return;
    }
    if (id.isEmpty() && m_topic >= 0)
        return;
    int t = 0, b = -1;
    if (id.isEmpty() || apple2help_find(id.toUtf8().constData(), &t, &b) != 0) {
        t = 0;
        b = -1;
    }
    if (!m_search->text().isEmpty())
        m_search->clear();
    showTopic(t, b, QString());
    m_filling = true;
    m_list->setCurrentRow(t);
    m_filling = false;
}

void HelpWindow::fillList()
{
    const QString q = m_search->text().trimmed();
    m_filling = true;
    m_list->clear();
    if (q.isEmpty()) {
        for (int t = 0; t < apple2help_topic_count(); t++) {
            auto *it = new QListWidgetItem(utf8(apple2help_topic_at(t)->title), m_list);
            it->setData(TOPIC_ROLE, t);
            it->setData(BLOCK_ROLE, -1);
        }
        if (m_topic >= 0)
            m_list->setCurrentRow(m_topic);
    } else {
        apple2help_hit hits[MAX_HITS];
        const int n = apple2help_search(ME, q.toUtf8().constData(), hits, MAX_HITS);
        for (int i = 0; i < n; i++) {
            const apple2help_topic *tp = apple2help_topic_at(hits[i].topic);
            const QString text = hits[i].block < 0
                ? utf8(tp->title)
                : utf8(apple2help_heading(&tp->blocks[hits[i].block])) + QLatin1Char('\n') + utf8(tp->title);
            auto *it = new QListWidgetItem(text, m_list);
            it->setData(TOPIC_ROLE, hits[i].topic);
            it->setData(BLOCK_ROLE, hits[i].block);
        }
        if (!n)
            (new QListWidgetItem(QStringLiteral("No matches"), m_list))->setFlags(Qt::NoItemFlags);
    }
    m_filling = false;
}

void HelpWindow::showTopic(int topic, int block, const QString &query)
{
    if (!apple2help_topic_at(topic))
        return;
    m_block = block;
    if (topic != m_topic || query != m_query) {
        m_topic = topic;
        m_query = query;
        render();
    } else {
        QTimer::singleShot(0, this, [this] { scrollToBlock(); });
    }
}

/* the block's HTML, the Breeze or Adwaita palette's colours on it */
QString HelpWindow::html() const
{
    const apple2help_topic *tp = apple2help_topic_at(m_topic);
    const QColor shade = mix(palette().color(QPalette::Base), palette().color(QPalette::Text), 0.10);
    const QString box = QStringLiteral("<table width=\"100%\" cellpadding=\"10\" bgcolor=\"%1\"><tr><td>")
                            .arg(shade.name());
    const QString boxEnd = QStringLiteral("</td></tr></table>");
    enum { NONE, UL, OL } list = NONE;
    bool sub = false, table = false;
    QString out = QStringLiteral("<h1>%1</h1>").arg(utf8(tp->title).toHtmlEscaped());

    auto closeAll = [&] {
        if (sub) out += QStringLiteral("</ul>");
        if (list == UL) out += QStringLiteral("</li></ul>");
        if (list == OL) out += QStringLiteral("</li></ol>");
        if (table) out += QStringLiteral("</table>");
        sub = table = false;
        list = NONE;
    };
    for (int i = 0; i < tp->nblocks; i++) {
        const apple2help_block &b = tp->blocks[i];
        if (!(b.platforms & ME))
            continue;
        const QString body = spansHtml(b, 0, b.nspans);
        switch (b.kind) {
        case APPLE2HELP_BULLET:
        case APPLE2HELP_NUMBERED: {
            const auto want = b.kind == APPLE2HELP_BULLET ? UL : OL;
            if (sub) out += QStringLiteral("</ul>");
            sub = false;
            if (list == want) {
                out += QStringLiteral("</li><li>");
            } else {
                closeAll();
                out += want == UL ? QStringLiteral("<ul><li>") : QStringLiteral("<ol><li>");
                list = want;
            }
            out += body;
            break;
        }
        case APPLE2HELP_BULLET2:
            if (list == NONE) {
                out += QStringLiteral("<ul><li>");
                list = UL;
            }
            if (!sub) out += QStringLiteral("<ul>");
            sub = true;
            out += QStringLiteral("<li>%1</li>").arg(body);
            break;
        case APPLE2HELP_TABLE_HEAD:
        case APPLE2HELP_TABLE_ROW: {
            if (b.kind == APPLE2HELP_TABLE_HEAD || !table) {
                closeAll();
                out += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"4\">");
                table = true;
            }
            const QString cell = b.kind == APPLE2HELP_TABLE_HEAD ? QStringLiteral("th align=\"left\"")
                                                                : QStringLiteral("td valign=\"top\"");
            const QString end = b.kind == APPLE2HELP_TABLE_HEAD ? QStringLiteral("th") : QStringLiteral("td");
            out += QStringLiteral("<tr>");
            int from = 0;
            for (int k = 0; k <= b.nspans; k++) {
                if (k < b.nspans && b.spans[k].kind != APPLE2HELP_CELL)
                    continue;
                out += QStringLiteral("<%1>%2</%3>").arg(cell, spansHtml(b, from, k), end);
                from = k + 1;
            }
            out += QStringLiteral("</tr>");
            break;
        }
        default:
            closeAll();
            switch (b.kind) {
            case APPLE2HELP_H2:
            case APPLE2HELP_H3:
                out += QStringLiteral("<%1><a name=\"%2\">%3</a></%1>")
                           .arg(b.kind == APPLE2HELP_H2 ? QStringLiteral("h2") : QStringLiteral("h3"),
                                utf8(b.id), utf8(apple2help_heading(&b)).toHtmlEscaped());
                break;
            case APPLE2HELP_NOTE: out += box + body + boxEnd; break;
            case APPLE2HELP_PRE:
                out += box + QStringLiteral("<pre>%1</pre>").arg(utf8(b.spans[0].text).toHtmlEscaped()) + boxEnd;
                break;
            default: out += QStringLiteral("<p>%1</p>").arg(body); break;
            }
        }
    }
    closeAll();
    return out;
}

void HelpWindow::render()
{
    if (m_topic < 0)
        return;
    const QColor shade = mix(palette().color(QPalette::Base), palette().color(QPalette::Text), 0.12);
    m_view->document()->setDefaultStyleSheet(QStringLiteral("code { background-color: %1; }").arg(shade.name()));
    m_view->setHtml(html());
    highlight();
    QTimer::singleShot(0, this, [this] { scrollToBlock(); });
}

void HelpWindow::highlight()
{
    QList<QTextEdit::ExtraSelection> sels;
    QTextCharFormat f;
    f.setBackground(QColor(0xf8, 0xe4, 0x5c));
    f.setForeground(QColor(0, 0, 0));
    for (const QString &w : m_query.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        QTextCursor c(m_view->document());
        while (!(c = m_view->document()->find(w, c)).isNull())
            sels.append({ c, f });
    }
    m_view->setExtraSelections(sels);
}

void HelpWindow::scrollToBlock()
{
    const apple2help_topic *tp = apple2help_topic_at(m_topic);
    if (tp && m_block >= 0 && m_block < tp->nblocks && tp->blocks[m_block].id)
        m_view->scrollToAnchor(utf8(tp->blocks[m_block].id));
    else
        m_view->verticalScrollBar()->setValue(0);
}

void HelpWindow::closeEvent(QCloseEvent *e)
{
    open = false;
    QWidget::closeEvent(e);
}

void HelpWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::PaletteChange)
        render();
    QWidget::changeEvent(e);
}

/* in the search: Down goes to the list, Escape clears */
bool HelpWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_search && e->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(e)->key();
        if (key == Qt::Key_Down && m_list->count()) {
            m_list->setFocus();
            if (!m_list->currentItem())
                m_list->setCurrentRow(0);
            return true;
        }
        if (key == Qt::Key_Escape && !m_search->text().isEmpty()) {
            m_search->clear();
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

} // namespace

void apple2HelpShow(QWidget *home, const QString &id)
{
    if (!g_help) {
        QWidget *root = home;
        while (root && root->parentWidget())
            root = root->parentWidget();
        g_help = new HelpWindow(root);
    }
    g_help->follow();
    g_help->showAt(id);
    g_help->open = true;
    g_help->show();
    g_help->raise();
    g_help->activateWindow();
}

void apple2HelpFor(QWidget *window)
{
    QWidget *w = window ? window->window() : nullptr;
    if (g_help && w == g_help) {
        g_help->findFocus();
        return;
    }
    const QString ctx = w ? w->property("apple2HelpContext").toString() : QString();
    apple2HelpShow(w, ctx.isEmpty() ? QStringLiteral("main") : ctx);
}

QAbstractButton *apple2HelpButton(const QString &context)
{
    auto *b = new QToolButton;
    b->setIcon(QIcon::fromTheme(QStringLiteral("help-contents"), QIcon::fromTheme(QStringLiteral("help-browser"))));
    if (b->icon().isNull())
        b->setText(QStringLiteral("Help"));
    b->setToolTip(QStringLiteral("Help"));
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    QObject::connect(b, &QToolButton::clicked, b, [b, context] {
        if (context.isEmpty())
            apple2HelpFor(b->window());
        else
            apple2HelpShow(b->window(), context);
    });
    return b;
}
