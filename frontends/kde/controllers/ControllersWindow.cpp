/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * What plugs into the Apple II's game port, as a player sees it. The Apple
 * Joystick IIe/IIc: a square case with two orange buttons, a raised block with
 * a pyramid well and a black stick tilting in it (BEIGE, as in the user's
 * photograph, or PLATINUM for the IIc); drag the stick and the machine's PDL0
 * and PDL1 follow the pointer, proportionally. Or the two Apple Hand
 * Controllers (paddles): a ribbed wedge each with a knob you drag round and an
 * orange button. Under them the console strip's Reset and Power Cycle. An
 * optional Apple //e keyboard (Platinum) closes the window: click a cap to
 * type it, Shift and Control latch, Caps Lock toggles, the Open and Option
 * (Closed) Apple caps are held and press the buttons.
 *
 * Everything is painted from the session's layout (apple2session_controls_
 * layout and _keyboard_layout, in layout units), so every frontend shows the
 * same parts in the same places; the colours are this file's, read off
 * photographs of the real hardware (DESIGN-apple2.md).
 *
 * Gestures act on mouse press AND release: a press present only for the
 * instant of a click would fall between the machine's polls. A button or a key
 * cap is held while the mouse is down on it (dragging a button off lets it go);
 * the stick follows the pointer wherever it is dragged; the knob turns to the
 * point and the wheel nudges it. One contact at a time. Hiding the window and
 * losing the focus let go of everything the mouse holds.
 *
 * What the machine sees held -- host keyboard (Alt is Open Apple), gamepads and
 * this window -- is lit in the accent, and the stick's cap and the knobs show
 * where PDL0 and PDL1 really stand, so the window doubles as an input tester
 * for a new pad.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts and gamepad names are elided, so neither
 * Map mode nor a long name resizes the window; only the Keyboard check box
 * does, and Joystick/Paddles does not. A singleton, hidden rather than
 * destroyed, so a half-done remap survives closing it. The timers run only
 * while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a button, a side of the
 * stick's well, a paddle's knob: left half turns it counter-clockwise, right
 * half clockwise, hub the pad axis that sets it, or a console button), then
 * press a host key or a gamepad button (an axis, for the hub). Rebinding steals
 * the key from whatever held it (named in the hint). Escape cancels.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <array>
#include <functional>
#include <utility>
#include <vector>

#include "bindings.h"

#include "../Accent.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"

namespace {

using apple2ctl::kGap;
using apple2ctl::kJoyH;
using apple2ctl::kMargin;
using apple2ctl::kPaddleUnit;
using apple2ctl::kStageH;
using apple2ctl::kStageW;
using apple2ctl::kStripH;
using apple2ctl::kTopPad;
using apple2ctl::kUnit;

constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;      /* an unpicked part while Map waits for a key */
constexpr float kWheelStep = 0.08f;   /* one wheel notch, of the knob's -1..1 */

/* ---- colours (photo-read or inferred, see DESIGN-apple2.md) ---------------------- */

struct Scheme {
    QColor caseTop, caseShade, wallTB, wallSide, floor, stick, stickHi, button, buttonDown, frame, logo;
    QColor paddleBody, paddleShade, knob, knobRib, knobIndex;
    QColor ink;           /* text on the case */
    QColor wallInk;       /* text on the well's walls */
};

Scheme scheme(int model)
{
    Scheme s;
    if (model == APPLE2_MODEL_IIC) {
        s.caseTop = QColor(0xEB, 0xE9, 0xE1);
        s.caseShade = QColor(0xCF, 0xCD, 0xC4);
        s.wallTB = QColor(0xBD, 0xBB, 0xB2);
        s.wallSide = QColor(0xCB, 0xC9, 0xC0);
        s.floor = QColor(0xA9, 0xA7, 0x9E);
        s.stick = QColor(0xA6, 0xA4, 0x99);
        s.stickHi = QColor(0xB4, 0xB2, 0xA7);
        s.button = QColor(0xF0, 0x64, 0x1C);
        s.buttonDown = QColor(0xC5, 0x4B, 0x0D);
        s.frame = QColor(0xB6, 0xB4, 0xAB);
        s.logo = QColor(0xD7, 0xD5, 0xCC);
        s.paddleBody = QColor(0xD5, 0xD3, 0xCA);
        s.paddleShade = QColor(0xBF, 0xBD, 0xB4);
        s.knob = QColor(0x8A, 0x88, 0x78);
        s.knobRib = QColor(0x70, 0x6E, 0x60);
        s.knobIndex = QColor(0xF2, 0xEF, 0xDE);
        s.ink = QColor(0x4A, 0x48, 0x42);
        s.wallInk = QColor(0x3A, 0x38, 0x34);
    } else {
        s.caseTop = QColor(0xE6, 0xDB, 0xBF);
        s.caseShade = QColor(0xCF, 0xC3, 0xA4);
        s.wallTB = QColor(0x4A, 0x4A, 0x4E);
        s.wallSide = QColor(0x5C, 0x5C, 0x60);
        s.floor = QColor(0x2A, 0x2A, 0x2E);
        s.stick = QColor(0x1D, 0x1D, 0x26);
        s.stickHi = QColor(0x44, 0x44, 0x52);
        s.button = QColor(0xF2, 0x6A, 0x1F);
        s.buttonDown = QColor(0xC9, 0x50, 0x0F);
        s.frame = QColor(0xB9, 0xAC, 0x8B);
        s.logo = QColor(0xD6, 0xCA, 0xAC);
        s.paddleBody = QColor(0xC9, 0xC3, 0xA8);
        s.paddleShade = QColor(0xB3, 0xAD, 0x92);
        s.knob = QColor(0x6F, 0x6C, 0x54);
        s.knobRib = QColor(0x5C, 0x5A, 0x44);
        s.knobIndex = QColor(0xE9, 0xE5, 0xD0);
        s.ink = QColor(0x5A, 0x50, 0x3C);
        s.wallInk = QColor(0xE6, 0xDF, 0xCC);
    }
    return s;
}

const QColor kMatTop(0x3E, 0x3F, 0x44);
const QColor kMatBottom(0x2A, 0x2B, 0x2F);
const QColor kMatEdge(0x16, 0x16, 0x18);
const QColor kMatInk(0xD9, 0xD6, 0xCB);
const QColor kStripFace(0xD9, 0xD6, 0xCB);
const QColor kStripHover(0xEA, 0xE8, 0xDF);
const QColor kStripEdge(0x8C, 0x8A, 0x80);
const QColor kStripInk(0x3A, 0x3A, 0x3C);

/* the keyboard (Platinum //e) */
const QColor kKbCase(0xCF, 0xCD, 0xC4);
const QColor kKbCaseEdge(0x9A, 0x98, 0x8F);
const QColor kKbAlpha(0xE4, 0xE2, 0xDA);
const QColor kKbMod(0xCC, 0xCA, 0xBF);
const QColor kKbReset(0xF4, 0xF3, 0xEE);
const QColor kKbSide(0x9C, 0x9A, 0x90);
const QColor kKbInk(0x2B, 0x2B, 0x2D);
const QColor kKbInkDim(0x66, 0x65, 0x60);

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont pixelFont(QFont fnt, int pixels, bool bold)
{
    fnt.setPixelSize(pixels);
    fnt.setBold(bold);
    return fnt;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(fnt);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const int size = fnt.pixelSize() > 0 ? fnt.pixelSize() : 10;
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5)
            break;
        fnt.setPixelSize(size - 1);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, Qt::AlignCenter, text);
}

/* A part's key as the window writes it, or empty for none. */
QString keyName(int target)
{
    char name[48] = "";
    const uint32_t keysym = bindings_target_keysym(target);
    if (keysym) apple2session_keysym_name(keysym, name, sizeof name);
    return local(name);
}

QString padName(int target)
{
    char name[48] = "";
    const int id = bindings_target_pad(target);
    if (id >= 0) apple2session_pad_input_name(id, name, sizeof name);
    return local(name);
}

/* The legend on a part: long key names shortened to fit (tooltips keep the full name). */
QString legendName(QString key)
{
    if (key.startsWith(QLatin1String("Keypad "))) return QStringLiteral("KP ") + key.mid(7);
    if (key.startsWith(QLatin1String("Left "))) return QStringLiteral("L ") + key.mid(5);
    if (key.startsWith(QLatin1String("Right "))) return QStringLiteral("R ") + key.mid(6);
    if (key == QLatin1String("Backspace")) return QStringLiteral("BkSp");
    if (key == QLatin1String("Escape")) return QStringLiteral("Esc");
    if (key == QLatin1String("Return")) return QStringLiteral("Ret");
    return key;
}

QString bindingText(int target)
{
    const QString key = keyName(target);
    return key.isEmpty() ? QStringLiteral("—") : legendName(key);
}

QString orNone(const QString &text) { return text.isEmpty() ? QStringLiteral("none") : text; }

QString bindingTip(int target)
{
    return QStringLiteral("%1\nKey: %2    Gamepad: %3")
        .arg(local(bindings_target_label(target)), orNone(keyName(target)), orNone(padName(target)));
}

bool isKnobAxisTarget(int target)
{
    return target >= APPLE2_TARGET_PADDLES &&
           (target - APPLE2_TARGET_PADDLES) % APPLE2_PADDLE_ACTS == APPLE2_PACT_KNOB;
}

QPointF at(const QPointF &origin, double unit, double x, double y)
{
    return origin + QPointF(x * unit, y * unit);
}

QRectF rectAt(const QPointF &origin, double unit, double x, double y, double w, double h)
{
    return QRectF(at(origin, unit, x, y), QSizeF(w * unit, h * unit));
}

/* the Apple: a body of two lobes with a leaf, a bite out of the right side when
 * filled; drawn inside a box of size s centred at c */
QPainterPath applePath(const QPointF &c, qreal s)
{
    QPainterPath body;
    body.addEllipse(c + QPointF(-0.27 * s, 0.06 * s), 0.44 * s, 0.46 * s);
    body.addEllipse(c + QPointF(0.27 * s, 0.06 * s), 0.44 * s, 0.46 * s);
    QPainterPath bite;
    bite.addEllipse(c + QPointF(0.74 * s, -0.02 * s), 0.2 * s, 0.2 * s);
    body = body.subtracted(bite);
    QPainterPath leaf;
    leaf.moveTo(c + QPointF(0.04 * s, -0.4 * s));
    leaf.quadTo(c + QPointF(0.12 * s, -0.95 * s), c + QPointF(0.52 * s, -0.82 * s));
    leaf.quadTo(c + QPointF(0.36 * s, -0.42 * s), c + QPointF(0.04 * s, -0.4 * s));
    return body.united(leaf);
}

/* A polygon with its corners rounded to a radius (a quadratic curve through each vertex). */
QPainterPath roundedPolygon(const QPolygonF &poly, qreal radius)
{
    QPainterPath path;
    const int count = poly.size();
    for (int idx = 0; idx < count; ++idx) {
        const QPointF prev = poly[(idx + count - 1) % count], cur = poly[idx], next = poly[(idx + 1) % count];
        QPointF toPrev = prev - cur, toNext = next - cur;
        const qreal lenPrev = std::hypot(toPrev.x(), toPrev.y()), lenNext = std::hypot(toNext.x(), toNext.y());
        const qreal rad = qMin(radius, qMin(lenPrev, lenNext) / 2);
        toPrev = toPrev / lenPrev * rad;
        toNext = toNext / lenNext * rad;
        if (idx == 0) path.moveTo(cur + toPrev);
        else path.lineTo(cur + toPrev);
        path.quadTo(cur, cur + toNext);
    }
    path.closeSubpath();
    return path;
}

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- Segment: a row of exclusive toggle buttons, the chosen one in the accent ---- */

class Segment : public QWidget {
public:
    Segment(const QStringList &labels, const QString &prefix, std::function<void(int)> onChange)
        : m_onChange(std::move(onChange))
    {
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        for (int idx = 0; idx < labels.size(); ++idx) {
            auto *button = new QPushButton(labels[idx]);
            button->setObjectName(prefix + QString::number(idx));
            button->setFocusPolicy(Qt::NoFocus);
            button->setCheckable(true);
            connect(button, &QPushButton::clicked, this, [this, idx] {
                setCurrent(idx);
                m_onChange(idx);
            });
            row->addWidget(button);
            m_buttons.push_back(button);
        }
        setCurrent(0);
    }

    void setCurrent(int idx)
    {
        for (int i = 0; i < (int)m_buttons.size(); ++i) {
            const QSignalBlocker block(m_buttons[(size_t)i]);
            m_buttons[(size_t)i]->setChecked(i == idx);
            apple2CtlSetAccent(m_buttons[(size_t)i], i == idx);
        }
        m_current = idx;
    }
    int current() const { return m_current; }

private:
    std::function<void(int)> m_onChange;
    std::vector<QPushButton *> m_buttons;
    int m_current = -1;
};

/* ---- StageWidget: the joystick or the paddles, and the console strip ---------------- */

class StageWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target); onHint: a message for the hint line */
    StageWidget(apple2session *session, std::function<void(int)> onPick, std::function<void(const QString &)> onHint)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setFixedSize(kStageW, kStageH);
        m_joy = apple2session_controls_layout(APPLE2_VIEW_JOYSTICK, &m_joyCount, nullptr, nullptr);
        m_paddles = apple2session_controls_layout(APPLE2_VIEW_PADDLES, &m_paddleCount, nullptr, nullptr);
        m_console = apple2session_controls_layout(APPLE2_VIEW_CONSOLE, &m_consoleCount, nullptr, nullptr);
        syncState();
    }

    /* Lets go of everything the mouse holds (also on focus loss and rebuild). */
    void releaseAll()
    {
        apple2session_pointer_release(m_session);
        m_hold = None;
        m_holdIdx = -1;
        m_holdView = -1;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* The mode and the colour scheme, as the session has them. */
    void syncState()
    {
        const int mode = apple2session_controller_mode(m_session);
        const int model = apple2session_controller_model(m_session);
        if (mode != m_mode || model != m_model) {
            m_mode = mode;
            m_model = model;
            m_scheme = scheme(model);
            update();
        }
    }

    /* What the machine sees held (keyboard, gamepads, the mouse), and where
     * PDL0/PDL1 and the knobs stand; repaints only on a change. */
    void pollHeld()
    {
        syncState();
        bool changed = false;
        auto track = [&](int target) {
            const bool now = apple2session_target_held(m_session, target) != 0;
            if (now != m_held[(size_t)target]) {
                m_held[(size_t)target] = now;
                changed = true;
            }
        };
        for (int act = 0; act < APPLE2_JOY_ACTS; ++act) track(APPLE2_TARGET_JOY(act));
        for (int pad = 0; pad < APPLE2SESSION_PADDLES; ++pad)
            for (int act = 0; act < APPLE2_PACT_KNOB; ++act) track(APPLE2_TARGET_PADDLE(pad, act));
        float x = 0, y = 0;
        apple2session_axes_get(m_session, &x, &y);
        if (x != m_axes[0] || y != m_axes[1]) {
            m_axes[0] = x;
            m_axes[1] = y;
            changed = true;
        }
        for (int pad = 0; pad < APPLE2SESSION_PADDLES; ++pad) {
            const float knob = apple2session_knob_get(m_session, pad);
            if (knob != m_knob[(size_t)pad]) {
                m_knob[(size_t)pad] = knob;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        paintMat(pnt);
        if (m_mode == APPLE2_MODE_PADDLES) paintPaddles(pnt);
        else paintJoystick(pnt);
        paintStrip(pnt);
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const QPointF pos = evt->position();
        const Hit hit = hitAt(pos);
        if (m_map != -2) {
            if (hit.idx >= 0) pick(hit, pos);
            return;
        }
        if (m_hold != None || hit.idx < 0) return;     /* one thing at a time */
        const apple2_control &ctl = layoutOf(hit.view)[hit.idx];
        switch (ctl.kind) {
        case APPLE2_CTL_BUTTON:
            m_hold = Part;
            m_holdView = hit.view;
            m_holdIdx = hit.idx;
            m_partDown = true;
            apple2session_press(m_session, ctl.target, 1);
            break;
        case APPLE2_CTL_STICK:
            m_hold = Stick;
            setStick(pos);
            break;
        case APPLE2_CTL_KNOB:
            m_hold = Dial;
            m_holdView = hit.view;
            m_holdIdx = hit.idx;
            dragDial(ctl, pos);
            break;
        case APPLE2_CTL_SYSBUTTON:
            m_hold = Strip;
            m_holdView = hit.view;
            m_holdIdx = hit.idx;
            m_partDown = true;
            break;
        default: return;
        }
        update();
    }

    /* Dragging off a button lets it go (and back on, holds it again); the stick
     * follows the pointer; the dial turns. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const QPointF pos = evt->position();
        const Hit hover = m_hold == None ? hitAt(pos) : Hit();
        if (hover.view != m_hover.view || hover.idx != m_hover.idx) {
            m_hover = hover;
            update();
        }
        if (m_hold == Stick) {
            setStick(pos);
            update();
        } else if (m_hold == Dial) {
            dragDial(layoutOf(m_holdView)[m_holdIdx], pos);
        } else if (m_hold == Part) {
            const Hit now = hitAt(pos);
            const bool over = now.view == m_holdView && now.idx == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                apple2session_press(m_session, layoutOf(m_holdView)[m_holdIdx].target, over ? 1 : 0);
                update();
            }
        } else if (m_hold == Strip) {
            const Hit now = hitAt(pos);
            const bool over = now.view == m_holdView && now.idx == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold == Part && m_partDown)
            apple2session_press(m_session, layoutOf(m_holdView)[m_holdIdx].target, 0);
        if (m_hold == Strip) {
            const Hit now = hitAt(evt->position());
            if (now.view == m_holdView && now.idx == m_holdIdx) {
                /* fires on release, like a real button: dragging off cancels. The
                 * main window's queue runs it, as it does for a key or a pad. */
                apple2session_sysaction_post(
                    m_session,
                    (apple2session_sysaction)(layoutOf(m_holdView)[m_holdIdx].target - APPLE2_TARGET_SYSACT_BASE));
            }
        }
        if (m_hold != None) releaseAll();
        m_hover = hitAt(evt->position());
        update();
    }

    void wheelEvent(QWheelEvent *evt) override
    {
        if (m_map != -2 || m_hold != None || m_mode != APPLE2_MODE_PADDLES) return;
        const Hit hit = hitAt(evt->position());
        if (hit.idx < 0) return;
        const apple2_control &ctl = layoutOf(hit.view)[hit.idx];
        if (ctl.kind != APPLE2_CTL_KNOB) return;
        const int steps = evt->angleDelta().y() / 120;
        if (!steps) return;
        apple2session_knob_nudge(m_session, ctl.port, (float)steps * kWheelStep);
        pollHeld();
        evt->accept();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover.idx != -1) {
            m_hover = Hit();
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Stick, Dial, Part, Strip };
    struct Hit {
        int view = -1;
        int idx = -1;
    };

    const apple2_control *layoutOf(int view) const
    {
        return view == APPLE2_VIEW_CONSOLE ? m_console : view == APPLE2_VIEW_PADDLES ? m_paddles : m_joy;
    }
    QPointF originOf(int view) const
    {
        return view == APPLE2_VIEW_CONSOLE ? apple2ctl::consoleOrigin()
             : view == APPLE2_VIEW_PADDLES ? apple2ctl::paddlesOrigin() : apple2ctl::joystickOrigin();
    }
    double unitOf(int view) const { return view == APPLE2_VIEW_PADDLES ? kPaddleUnit : kUnit; }
    int activeView() const { return m_mode == APPLE2_MODE_PADDLES ? APPLE2_VIEW_PADDLES : APPLE2_VIEW_JOYSTICK; }

    QRectF rectOf(int view, const apple2_control &ctl) const
    {
        return rectAt(originOf(view), unitOf(view), ctl.xpos, ctl.ypos, ctl.width, ctl.height);
    }

    Hit hitAt(const QPointF &pos) const
    {
        const int views[2] = { (int)APPLE2_VIEW_CONSOLE, activeView() };
        for (int view : views) {
            const QPointF origin = originOf(view);
            const double unit = unitOf(view);
            const int idx = apple2session_controls_hit(view, float((pos.x() - origin.x()) / unit),
                                                       float((pos.y() - origin.y()) / unit));
            if (idx >= 0) {
                Hit hit;
                hit.view = view;
                hit.idx = idx;
                return hit;
            }
        }
        return Hit();
    }

    /* The stick follows the pointer, wherever it is dragged to. */
    void setStick(const QPointF &pos)
    {
        const QPointF origin = apple2ctl::joystickOrigin();
        float x = 0, y = 0;
        apple2session_joystick_from_point(float((pos.x() - origin.x()) / kUnit),
                                          float((pos.y() - origin.y()) / kUnit), &x, &y);
        apple2session_pointer_set(m_session, x, y);
        pollHeld();
    }

    /* The knob to the angle of pos (nothing in the dead centre). */
    void dragDial(const apple2_control &ctl, const QPointF &pos)
    {
        const QPointF centre = at(apple2ctl::paddlesOrigin(), kPaddleUnit, ctl.cx, ctl.cy);
        float value = 0;
        if (apple2session_knob_from_point(pos.x() - centre.x(), pos.y() - centre.y(), ctl.reach * kPaddleUnit,
                                          &value)) {
            apple2session_knob_set(m_session, ctl.port, value);
            pollHeld();
        }
    }

    /* Map mode: a click on a part picks its target. The stick has four (the side
     * of the well that was clicked), a knob three (left half, right half, hub). */
    void pick(const Hit &hit, const QPointF &pos)
    {
        const apple2_control &ctl = layoutOf(hit.view)[hit.idx];
        if (ctl.kind == APPLE2_CTL_STICK) {
            const QPointF origin = apple2ctl::joystickOrigin();
            const double dx = (pos.x() - origin.x()) / kUnit - (ctl.xpos + ctl.width / 2);
            const double dy = (pos.y() - origin.y()) / kUnit - (ctl.ypos + ctl.height / 2);
            if (std::hypot(dx, dy) < 0.6) {
                m_onHint(QStringLiteral("Click toward a side of the well: left, right, up or down."));
                return;
            }
            const int act = std::abs(dx) > std::abs(dy) ? (dx < 0 ? APPLE2_ACT_LEFT : APPLE2_ACT_RIGHT)
                                                        : (dy < 0 ? APPLE2_ACT_UP : APPLE2_ACT_DOWN);
            m_onPick(APPLE2_TARGET_JOY(act));
        } else if (ctl.kind == APPLE2_CTL_KNOB) {
            const QPointF centre = at(apple2ctl::paddlesOrigin(), kPaddleUnit, ctl.cx, ctl.cy);
            const QPointF delta = pos - centre;
            if (std::hypot(delta.x(), delta.y()) < ctl.reach * kPaddleUnit * 0.35)
                m_onPick(APPLE2_TARGET_PADDLE(ctl.port, APPLE2_PACT_KNOB));
            else
                m_onPick(APPLE2_TARGET_PADDLE(ctl.port, delta.x() < 0 ? APPLE2_PACT_CCW : APPLE2_PACT_CW));
        } else if (ctl.target >= 0) {
            m_onPick(ctl.target);
        }
    }

    QString tipAt(const QPoint &pos) const
    {
        const Hit hit = hitAt(pos);
        if (hit.idx < 0) return QString();
        const apple2_control &ctl = layoutOf(hit.view)[hit.idx];
        if (ctl.kind == APPLE2_CTL_KNOB) {
            const int pad = ctl.port;
            return QStringLiteral("Paddle %1: drag round to turn it, scroll to nudge it\n"
                                  "Counter-clockwise: %2    Clockwise: %3    Gamepad axis: %4")
                .arg(pad)
                .arg(orNone(keyName(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CCW))),
                     orNone(keyName(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CW))),
                     orNone(padName(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_KNOB))));
        }
        if (ctl.kind == APPLE2_CTL_STICK) {
            return QStringLiteral("Drag to move the stick\nLeft: %1  Right: %2  Up: %3  Down: %4")
                .arg(orNone(keyName(APPLE2_TARGET_JOY(APPLE2_ACT_LEFT))),
                     orNone(keyName(APPLE2_TARGET_JOY(APPLE2_ACT_RIGHT))),
                     orNone(keyName(APPLE2_TARGET_JOY(APPLE2_ACT_UP))),
                     orNone(keyName(APPLE2_TARGET_JOY(APPLE2_ACT_DOWN))));
        }
        if (ctl.target >= 0) {
            QString tip = bindingTip(ctl.target);
            if (ctl.kind == APPLE2_CTL_BUTTON && m_mode == APPLE2_MODE_JOYSTICK)
                tip += ctl.target == APPLE2_TARGET_JOY(APPLE2_ACT_BTN0) ? QStringLiteral("\n(also the Open Apple key: Alt)")
                                                                        : QStringLiteral("\n(also the Closed Apple key: Right Alt)");
            return tip;
        }
        return QString();
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    bool heldTarget(int target) const { return target >= 0 && m_held[(size_t)target]; }

    /* While Map waits for a key, every part but the picked one steps back. */
    bool dimmed(int target) const { return m_map >= 0 && target != m_map; }
    bool dimmedAny(std::initializer_list<int> targets) const
    {
        if (m_map < 0) return false;
        for (int target : targets) if (target == m_map) return false;
        return true;
    }
    bool picked(int target) const { return mapping() && target >= 0 && target == m_map; }

    /* ---- painting ---- */

    void paintMat(QPainter &pnt)
    {
        const QRectF mat(2, 2, width() - 4, height() - 4);
        QLinearGradient grad(mat.topLeft(), mat.bottomLeft());
        grad.setColorAt(0, kMatTop);
        grad.setColorAt(1, kMatBottom);
        pnt.setPen(QPen(kMatEdge, 2));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(mat, 14, 14);
    }

    void paintJoystick(QPainter &pnt)
    {
        const QPointF origin = apple2ctl::joystickOrigin();
        const double unit = kUnit;
        for (int idx = 0; idx < m_joyCount; ++idx) {
            const apple2_control &ctl = m_joy[idx];
            switch (ctl.kind) {
            case APPLE2_CTL_PART: paintJoyPart(pnt, origin, unit, ctl); break;
            case APPLE2_CTL_BUTTON: paintButton(pnt, origin, unit, ctl, QString()); break;
            case APPLE2_CTL_STICK: paintStick(pnt, origin, unit, ctl); break;
            default: break;
            }
        }
    }

    void paintJoyPart(QPainter &pnt, const QPointF &origin, double unit, const apple2_control &ctl)
    {
        const QRectF box = rectAt(origin, unit, ctl.xpos, ctl.ypos, ctl.width, ctl.height);
        switch (ctl.part) {
        case APPLE2_PART_JOY_BASE: {
            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            grad.setColorAt(0, m_scheme.caseTop.lighter(103));
            grad.setColorAt(1, m_scheme.caseTop.darker(104));
            pnt.setPen(QPen(m_scheme.caseShade.darker(115), 1.2));
            pnt.setBrush(grad);
            pnt.drawRoundedRect(box, 0.35 * unit, 0.35 * unit);
            /* the seam between the shells, near the bottom */
            pnt.setPen(QPen(m_scheme.caseShade, 1.2));
            const double seam = box.top() + 7.7 * unit;
            pnt.drawLine(QPointF(box.left() + 0.3 * unit, seam), QPointF(box.right() - 0.3 * unit, seam));
            break;
        }
        case APPLE2_PART_JOY_BLOCK: {
            /* it stands proud of the base: a soft shadow on its left, a lighter top edge */
            QLinearGradient shadow(box.left() - 0.3 * unit, 0, box.left(), 0);
            shadow.setColorAt(0, QColor(0, 0, 0, 0));
            shadow.setColorAt(1, QColor(0, 0, 0, 46));
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(shadow);
            pnt.drawRect(QRectF(box.left() - 0.3 * unit, box.top() + 0.3 * unit, 0.3 * unit, box.height() - 0.6 * unit));
            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            grad.setColorAt(0, m_scheme.caseTop.lighter(106));
            grad.setColorAt(1, m_scheme.caseTop.darker(102));
            pnt.setPen(QPen(m_scheme.caseShade.darker(112), 1.2));
            pnt.setBrush(grad);
            pnt.drawRoundedRect(box, 0.3 * unit, 0.3 * unit);
            pnt.setPen(QPen(m_scheme.caseTop.lighter(118), 1.5));
            pnt.drawLine(QPointF(box.left() + 0.35 * unit, box.top() + 1.5), QPointF(box.right() - 0.35 * unit, box.top() + 1.5));
            break;
        }
        case APPLE2_PART_JOY_WELL: paintWell(pnt, origin, unit, box); break;
        case APPLE2_PART_JOY_FLOOR: break;     /* painted with the well, which it sits in */
        case APPLE2_PART_JOY_LOGO: {
            pnt.setPen(QPen(m_scheme.logo.darker(112), 1.2));
            pnt.setBrush(m_scheme.logo);
            pnt.drawRoundedRect(box, 0.16 * unit, 0.16 * unit);
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(m_scheme.caseShade.darker(125));
            pnt.drawPath(applePath(box.center() + QPointF(0, 0.04 * unit), 0.3 * unit));
            break;
        }
        default: break;
        }
    }

    /* The pyramid: four sloping walls down to a small floor. */
    void paintWell(QPainter &pnt, const QPointF &origin, double unit, const QRectF &box)
    {
        const QRectF pit = rectAt(origin, unit, 4.5, 5.6, 1.0, 1.0);
        const QPointF tl = box.topLeft(), tr = box.topRight(), br = box.bottomRight(), bl = box.bottomLeft();
        const QPointF ftl = pit.topLeft(), ftr = pit.topRight(), fbr = pit.bottomRight(), fbl = pit.bottomLeft();
        pnt.setPen(QPen(m_scheme.wallTB.darker(150), 1));
        pnt.setBrush(m_scheme.wallTB);                                   /* top */
        pnt.drawPolygon(QPolygonF({ tl, tr, ftr, ftl }));
        pnt.setBrush(m_scheme.wallTB.darker(112));                       /* bottom */
        pnt.drawPolygon(QPolygonF({ bl, br, fbr, fbl }));
        pnt.setBrush(m_scheme.wallSide);                                 /* left */
        pnt.drawPolygon(QPolygonF({ tl, ftl, fbl, bl }));
        pnt.setBrush(m_scheme.wallSide.darker(108));                     /* right */
        pnt.drawPolygon(QPolygonF({ tr, br, fbr, ftr }));
        pnt.setPen(QPen(m_scheme.floor.darker(130), 1));
        pnt.setBrush(m_scheme.floor);
        pnt.drawRect(pit);
        if (showBind()) paintWellLegends(pnt, box);
    }

    /* The four directions' keys on the walls, in Map mode and with Show bindings. */
    void paintWellLegends(QPainter &pnt, const QRectF &box)
    {
        const struct { int act; QRectF area; } sides[] = {
            { APPLE2_ACT_UP, QRectF(box.left() + 0.9 * kUnit, box.top() + 0.06 * kUnit, box.width() - 1.8 * kUnit, 0.7 * kUnit) },
            { APPLE2_ACT_DOWN, QRectF(box.left() + 0.9 * kUnit, box.bottom() - 0.76 * kUnit, box.width() - 1.8 * kUnit, 0.7 * kUnit) },
            { APPLE2_ACT_LEFT, QRectF(box.left() + 0.04 * kUnit, box.center().y() - 0.35 * kUnit, 0.9 * kUnit, 0.7 * kUnit) },
            { APPLE2_ACT_RIGHT, QRectF(box.right() - 0.94 * kUnit, box.center().y() - 0.35 * kUnit, 0.9 * kUnit, 0.7 * kUnit) },
        };
        for (const auto &side : sides) {
            const int target = APPLE2_TARGET_JOY(side.act);
            if (picked(target)) {
                pnt.setPen(Qt::NoPen);
                QColor glow = apple2CtlAccent();
                glow.setAlpha(150);
                pnt.setBrush(glow);
                pnt.drawRoundedRect(side.area, 4, 4);
            }
            pnt.setPen(picked(target) ? apple2CtlAccentInk() : m_scheme.wallInk);
            pnt.setOpacity(dimmed(target) ? kDimAlpha : 1.0);
            drawFitted(pnt, side.area, bindingText(target), pixelFont(font(), 11, true));
            pnt.setOpacity(1.0);
        }
    }

    /* A fire button (the joystick's or a paddle's): a frame, the orange square pushed
     * down while held, the accent ring when anyone holds it, the key in Map mode. */
    void paintButton(QPainter &pnt, const QPointF &origin, double unit, const apple2_control &ctl, const QString &)
    {
        const QRectF box = rectAt(origin, unit, ctl.xpos, ctl.ypos, ctl.width, ctl.height);
        const bool down = heldTarget(ctl.target);
        const bool lit = picked(ctl.target);
        pnt.setOpacity(dimmed(ctl.target) ? kDimAlpha : 1.0);
        pnt.setPen(QPen(m_scheme.frame.darker(125), 1));
        pnt.setBrush(m_scheme.frame);
        pnt.drawRoundedRect(box.adjusted(-0.07 * unit, -0.07 * unit, 0.07 * unit, 0.07 * unit), 0.16 * unit, 0.16 * unit);
        QRectF face = box;
        if (down) face.translate(0, 0.08 * unit);
        QLinearGradient grad(face.topLeft(), face.bottomLeft());
        const QColor base = down || lit ? m_scheme.buttonDown : m_scheme.button;
        grad.setColorAt(0, base.lighter(112));
        grad.setColorAt(1, base);
        pnt.setPen(QPen(base.darker(135), 1));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(face, 0.12 * unit, 0.12 * unit);
        if (down || lit) {
            pnt.setPen(QPen(apple2CtlAccent(), 2.5));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawRoundedRect(box.adjusted(-0.11 * unit, -0.11 * unit, 0.11 * unit, 0.11 * unit), 0.2 * unit, 0.2 * unit);
        }
        if (showBind()) {
            pnt.setPen(QColor(0xFF, 0xF4, 0xE8));
            drawFitted(pnt, face.adjusted(2, 1, -2, -1), bindingText(ctl.target), pixelFont(font(), 10, true));
        }
        pnt.setOpacity(1.0);
    }

    /* The shaft and cap, where the machine's PDL0/PDL1 really are: it follows the
     * mouse, the keys and a gamepad's stick alike. */
    void paintStick(QPainter &pnt, const QPointF &origin, double unit, const apple2_control &ctl)
    {
        const QPointF socket = at(origin, unit, ctl.cx, ctl.cy);
        const QPointF cap = at(origin, unit, ctl.capx + m_axes[0] * ctl.reach, ctl.capy + m_axes[1] * ctl.reach);
        const bool lit = m_held[APPLE2_TARGET_JOY(APPLE2_ACT_LEFT)] || m_held[APPLE2_TARGET_JOY(APPLE2_ACT_RIGHT)] ||
                         m_held[APPLE2_TARGET_JOY(APPLE2_ACT_UP)] || m_held[APPLE2_TARGET_JOY(APPLE2_ACT_DOWN)] ||
                         m_hold == Stick;
        pnt.setOpacity(dimmedAny({ APPLE2_TARGET_JOY(APPLE2_ACT_LEFT), APPLE2_TARGET_JOY(APPLE2_ACT_RIGHT),
                                   APPLE2_TARGET_JOY(APPLE2_ACT_UP), APPLE2_TARGET_JOY(APPLE2_ACT_DOWN) })
                           ? kDimAlpha : 1.0);
        /* the socket and the shaft */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(m_scheme.stick.darker(140));
        pnt.drawEllipse(socket, 0.36 * unit, 0.3 * unit);
        pnt.setPen(QPen(m_scheme.stick, 0.34 * unit, Qt::SolidLine, Qt::RoundCap));
        pnt.drawLine(socket, cap);
        /* the cap: a rounded capsule along the shaft */
        QPointF dir = cap - socket;
        const double len = std::hypot(dir.x(), dir.y());
        dir = len > 0.001 ? dir / len : QPointF(0, -1);
        const QPointF tail = cap - dir * 0.55 * unit, head = cap + dir * 0.55 * unit;
        if (lit) {
            pnt.setPen(QPen(apple2CtlAccent(), 0.62 * unit + 5, Qt::SolidLine, Qt::RoundCap));
            pnt.drawLine(tail, head);
        }
        pnt.setPen(QPen(m_scheme.stick, 0.5 * unit, Qt::SolidLine, Qt::RoundCap));
        pnt.drawLine(tail, head);
        pnt.setPen(QPen(m_scheme.stickHi, 0.14 * unit, Qt::SolidLine, Qt::RoundCap));
        const QPointF side(-dir.y() * 0.12 * unit, dir.x() * 0.12 * unit);
        pnt.drawLine(tail + side, head + side);
        pnt.setOpacity(1.0);
    }

    void paintPaddles(QPainter &pnt)
    {
        const QPointF origin = apple2ctl::paddlesOrigin();
        const double unit = kPaddleUnit;
        for (int idx = 0; idx < m_paddleCount; ++idx) {
            const apple2_control &ctl = m_paddles[idx];
            switch (ctl.kind) {
            case APPLE2_CTL_PART: paintPaddleBody(pnt, origin, unit, ctl); break;
            case APPLE2_CTL_KNOB: paintKnob(pnt, origin, unit, ctl); break;
            case APPLE2_CTL_BUTTON: paintButton(pnt, origin, unit, ctl, QString()); break;
            case APPLE2_CTL_CAPTION: {
                pnt.setPen(kMatInk);
                const QRectF box = rectAt(origin, unit, ctl.xpos, ctl.ypos, ctl.width, ctl.height);
                drawFitted(pnt, box, local(ctl.label), pixelFont(font(), 12, true));
                break;
            }
            default: break;
            }
        }
    }

    void paintPaddleBody(QPainter &pnt, const QPointF &origin, double unit, const apple2_control &ctl)
    {
        const double ox = ctl.xpos;
        QPolygonF poly;
        const double pts[][2] = { { 0, 0.6 }, { 5.2, 0 }, { 6, 0.9 }, { 6, 2.4 }, { 5, 3 }, { 0, 2.6 } };
        for (const auto &pt : pts) poly << at(origin, unit, ox + pt[0], ctl.ypos + pt[1]);
        QLinearGradient grad(at(origin, unit, ox, 0), at(origin, unit, ox, 3));
        grad.setColorAt(0, m_scheme.paddleBody.lighter(106));
        grad.setColorAt(1, m_scheme.paddleBody.darker(104));
        pnt.setPen(QPen(m_scheme.paddleShade.darker(125), 1.3));
        pnt.setBrush(grad);
        pnt.drawPath(roundedPolygon(poly, 0.25 * unit));
        /* the fine ribs along the lower edge */
        pnt.setPen(QPen(m_scheme.paddleShade, 1.3));
        for (int rib = 0; rib < 11; ++rib) {
            const double x = ox + 0.55 + rib * 0.42;
            if (x > ox + 4.6) break;
            const double edge = 2.6 + 0.08 * (x - ox);
            pnt.drawLine(at(origin, unit, x, ctl.ypos + edge - 0.42), at(origin, unit, x, ctl.ypos + edge - 0.12));
        }
    }

    /* The knob: a ribbed rim, a flat top with an index at the pot's angle, the end
     * stops of its 270 degrees, an accent ring while the machine turns it. */
    void paintKnob(QPainter &pnt, const QPointF &origin, double unit, const apple2_control &ctl)
    {
        const int pad = ctl.port;
        const QPointF centre = at(origin, unit, ctl.cx, ctl.cy);
        const double radius = ctl.reach * unit;
        const bool turning = heldTarget(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CCW)) ||
                             heldTarget(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CW)) || (m_hold == Dial && m_holdIdx >= 0 &&
                             layoutOf(m_holdView)[m_holdIdx].port == pad);
        pnt.setOpacity(dimmedAny({ APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CCW), APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CW),
                                   APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_KNOB) }) ? kDimAlpha : 1.0);
        /* the end stops */
        pnt.setPen(QPen(m_scheme.paddleShade.darker(130), 1.6));
        for (double deg : { -135.0, 135.0 }) {
            const double rad = qDegreesToRadians(deg - 90.0);
            pnt.drawLine(centre + QPointF(std::cos(rad), std::sin(rad)) * (radius + 3),
                         centre + QPointF(std::cos(rad), std::sin(rad)) * (radius + 8));
        }
        if (turning) {
            pnt.setPen(QPen(apple2CtlAccent(), 3));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawEllipse(centre, radius + 2.5, radius + 2.5);
        }
        /* the ribbed rim */
        pnt.setPen(QPen(m_scheme.knobRib.darker(130), 1));
        pnt.setBrush(m_scheme.knob.darker(112));
        pnt.drawEllipse(centre, radius, radius);
        pnt.setPen(QPen(m_scheme.knobRib, 1.6));
        for (int tooth = 0; tooth < 24; ++tooth) {
            const double rad = qDegreesToRadians(tooth * 15.0);
            pnt.drawLine(centre + QPointF(std::cos(rad), std::sin(rad)) * (radius * 0.86),
                         centre + QPointF(std::cos(rad), std::sin(rad)) * (radius * 0.99));
        }
        /* the flat top */
        QLinearGradient grad(centre - QPointF(radius * 0.6, radius * 0.6), centre + QPointF(radius * 0.6, radius * 0.6));
        grad.setColorAt(0, m_scheme.knob.lighter(122));
        grad.setColorAt(1, m_scheme.knob);
        pnt.setPen(QPen(m_scheme.knobRib, 1));
        pnt.setBrush(grad);
        pnt.drawEllipse(centre, radius * 0.78, radius * 0.78);
        /* the index at the pot's angle: clockwise from straight up */
        const double rad = qDegreesToRadians(apple2session_knob_angle(m_knob[(size_t)pad]) - 90.0);
        const QPointF dir(std::cos(rad), std::sin(rad));
        pnt.setPen(QPen(m_scheme.knobIndex, 3, Qt::SolidLine, Qt::RoundCap));
        pnt.drawLine(centre + dir * (radius * 0.28), centre + dir * (radius * 0.7));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(m_scheme.knobIndex);
        pnt.drawEllipse(centre, 2.6, 2.6);
        if (showBind()) {
            pnt.setPen(QColor(0xFF, 0xFA, 0xEA));
            const QString ccw = bindingText(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CCW));
            const QString cw = bindingText(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CW));
            drawFitted(pnt, QRectF(centre.x() - radius * 0.74, centre.y() + radius * 0.16, radius * 0.72, 13), ccw,
                       pixelFont(font(), 9, true));
            drawFitted(pnt, QRectF(centre.x() + radius * 0.02, centre.y() + radius * 0.16, radius * 0.72, 13), cw,
                       pixelFont(font(), 9, true));
            if (picked(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CCW)) || picked(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_CW)) ||
                picked(APPLE2_TARGET_PADDLE(pad, APPLE2_PACT_KNOB))) {
                pnt.setPen(QPen(apple2CtlAccent(), 3));
                pnt.setBrush(Qt::NoBrush);
                pnt.drawEllipse(centre, radius + 2.5, radius + 2.5);
            }
        }
        pnt.setOpacity(1.0);
    }

    /* The console strip: Reset and Power Cycle, light plastic buttons. */
    void paintStrip(QPainter &pnt)
    {
        const QPointF origin = apple2ctl::consoleOrigin();
        pnt.setPen(kMatInk);
        pnt.setOpacity(0.7);
        pnt.drawText(QRectF(kMargin, origin.y(), 120, kStripH), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Apple II"));
        pnt.setOpacity(1.0);
        for (int idx = 0; idx < m_consoleCount; ++idx) {
            const apple2_control &ctl = m_console[idx];
            const QRectF box = rectAt(origin, kUnit, ctl.xpos, ctl.ypos, ctl.width, ctl.height);
            const bool down = m_hold == Strip && m_holdView == APPLE2_VIEW_CONSOLE && m_holdIdx == idx && m_partDown;
            const bool hover = m_hover.view == APPLE2_VIEW_CONSOLE && m_hover.idx == idx;
            const bool lit = picked(ctl.target);
            pnt.setOpacity(dimmed(ctl.target) ? kDimAlpha : 1.0);
            pnt.setPen(QPen(lit || down ? apple2CtlAccent() : kStripEdge, lit || down ? 2.5 : 1.3));
            pnt.setBrush(down ? kStripFace.darker(112) : hover ? kStripHover : kStripFace);
            pnt.drawRoundedRect(box.adjusted(0, down ? 1 : 0, 0, down ? 1 : 0), 6, 6);
            pnt.setPen(kStripInk);
            if (showBind()) {
                drawFitted(pnt, QRectF(box.left(), box.top() + 1, box.width(), box.height() * 0.55), local(ctl.label),
                           pixelFont(font(), 12, true));
                drawFitted(pnt, QRectF(box.left(), box.top() + box.height() * 0.5, box.width(), box.height() * 0.45),
                           bindingText(ctl.target), pixelFont(font(), 9, false));
            } else {
                drawFitted(pnt, box, local(ctl.label), pixelFont(font(), 13, true));
            }
            pnt.setOpacity(1.0);
        }
    }

    apple2session *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;
    const apple2_control *m_joy = nullptr, *m_paddles = nullptr, *m_console = nullptr;
    int m_joyCount = 0, m_paddleCount = 0, m_consoleCount = 0;
    int m_mode = -1, m_model = -1;
    Scheme m_scheme = scheme(APPLE2_MODEL_IIE);
    std::array<bool, APPLE2_TARGET_COUNT> m_held{};
    float m_axes[2] = { 0, 0 };
    std::array<float, APPLE2SESSION_PADDLES> m_knob{};
    Hold m_hold = None;
    int m_holdView = -1, m_holdIdx = -1;
    bool m_partDown = false;
    Hit m_hover;
    int m_map = -2;
    bool m_showBindings = false;
};

/* ---- KeyboardWidget: the Apple //e's keys ----------------------------------------- */

class KeyboardWidget : public QWidget {
public:
    explicit KeyboardWidget(apple2session *session) : m_session(session)
    {
        setFocusPolicy(Qt::NoFocus);
        float wide = 0, tall = 0;
        m_caps = apple2session_keyboard_layout(&m_count, &wide, &tall);
        m_state.assign((size_t)m_count, 0);
        setFixedSize(qCeil(wide * kUnit) + 2 * kMargin, qCeil(tall * kUnit) + 2 * kMargin);
    }

    /* Lets go of every cap that is down and every latch but Caps Lock. */
    void releaseAll()
    {
        apple2session_keyboard_release(m_session);
        m_down = -1;
        poll();
        update();
    }

    /* The caps' lit and latched states; repaints only on a change. */
    void poll()
    {
        bool changed = false;
        for (int idx = 0; idx < m_count; ++idx) {
            const int now = apple2session_keyboard_held(m_session, idx);
            if (now != m_state[(size_t)idx]) {
                m_state[(size_t)idx] = now;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        const QRectF body(2, 2, width() - 4, height() - 4);
        QLinearGradient grad(body.topLeft(), body.bottomLeft());
        grad.setColorAt(0, kKbCase.lighter(104));
        grad.setColorAt(1, kKbCase.darker(104));
        pnt.setPen(QPen(kKbCaseEdge, 1.5));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(body, 9, 9);
        for (int idx = 0; idx < m_count; ++idx) paintCap(pnt, idx);
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton || m_down >= 0) return;
        const int idx = capAt(evt->position());
        if (idx < 0) return;
        m_down = idx;
        apple2session_keyboard_press(m_session, idx, 1);
        poll();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton || m_down < 0) return;
        apple2session_keyboard_press(m_session, m_down, 0);
        m_down = -1;
        poll();
        update();
    }

private:
    QRectF capRect(int idx) const
    {
        const apple2_keycap &cap = m_caps[idx];
        return QRectF(kMargin + cap.xpos * kUnit, kMargin + cap.ypos * kUnit, cap.width * kUnit, cap.height * kUnit);
    }

    int capAt(const QPointF &pos) const
    {
        for (int idx = m_count - 1; idx >= 0; --idx)
            if (capRect(idx).contains(pos)) return idx;
        return -1;
    }

    static bool isArrow(const apple2_keycap &cap)
    {
        return cap.kind == APPLE2_KEYCAP_SPECIAL && cap.keysym >= APPLE2SESSION_KEYSYM_LEFT &&
               cap.keysym <= APPLE2SESSION_KEYSYM_DOWN;
    }

    void paintCap(QPainter &pnt, int idx)
    {
        const apple2_keycap &cap = m_caps[idx];
        const int state = m_state[(size_t)idx];
        const bool lit = (state & 1) != 0, latched = (state & 2) != 0;
        QRectF face = capRect(idx).adjusted(1.5, 1.5, -1.5, -2.5);
        const QColor base = cap.kind == APPLE2_KEYCAP_RESET ? kKbReset
                          : cap.kind == APPLE2_KEYCAP_CHAR || cap.kind == APPLE2_KEYCAP_SPACE ? kKbAlpha : kKbMod;
        /* the cap's side, then its face (pushed down while it is) */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kKbSide);
        pnt.drawRoundedRect(face.translated(0, 2), 4, 4);
        if (lit) face.translate(0, 1.5);
        QLinearGradient grad(face.topLeft(), face.bottomLeft());
        const QColor top = lit ? apple2CtlAccent().lighter(125) : base.lighter(106);
        grad.setColorAt(0, top);
        grad.setColorAt(1, lit ? apple2CtlAccent() : base.darker(103));
        pnt.setPen(QPen(lit ? apple2CtlAccentDark() : kKbSide, 1));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(face, 4, 4);

        pnt.setPen(lit ? apple2CtlAccentInk() : kKbInk);
        const QFont fnt = font();
        if (cap.kind == APPLE2_KEYCAP_APPLE) {
            /* an outline apple on Open, a filled one on Option (Closed), the word below */
            const QPointF c(face.center().x(), face.top() + face.height() * 0.4);
            const qreal s = face.height() * 0.36;
            const QPainterPath apple = applePath(c, s);
            if (cap.keysym == 0) {
                pnt.setBrush(Qt::NoBrush);
                pnt.setPen(QPen(lit ? apple2CtlAccentInk() : kKbInk, 1.4));
            } else {
                pnt.setBrush(lit ? apple2CtlAccentInk() : kKbInk);
                pnt.setPen(Qt::NoPen);
            }
            pnt.drawPath(apple);
            pnt.setPen(lit ? apple2CtlAccentInk() : kKbInkDim);
            drawFitted(pnt, QRectF(face.left(), face.top() + face.height() * 0.68, face.width(), face.height() * 0.3),
                       cap.keysym == 0 ? QStringLiteral("Open") : QStringLiteral("Option"), pixelFont(fnt, 8, false));
        } else if (isArrow(cap)) {
            paintArrow(pnt, face, cap.keysym, lit ? apple2CtlAccentInk() : kKbInk);
        } else if (cap.shifted && cap.kind == APPLE2_KEYCAP_CHAR) {
            /* two printed legends: the shifted one above the base one */
            drawFitted(pnt, QRectF(face.left(), face.top() + 1, face.width(), face.height() * 0.46), local(cap.shifted),
                       pixelFont(fnt, 11, false));
            drawFitted(pnt, QRectF(face.left(), face.top() + face.height() * 0.42, face.width(), face.height() * 0.56),
                       local(cap.legend), pixelFont(fnt, 13, true));
        } else if (cap.kind == APPLE2_KEYCAP_CHAR) {
            drawFitted(pnt, face, local(cap.legend), pixelFont(fnt, 14, true));
        } else if (cap.kind != APPLE2_KEYCAP_SPACE) {
            /* Esc, Tab, Delete, Return, Control, Shift, Caps Lock, the Reset cap */
            drawFitted(pnt, face.adjusted(2, 0, -2, 0), local(cap.legend), pixelFont(fnt, 10, true));
        }
        /* a latch that is on carries a dot */
        if (latched) {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(lit ? apple2CtlAccentInk() : apple2CtlAccent());
            pnt.drawEllipse(QPointF(face.right() - 6, face.top() + 6), 3.2, 3.2);
        }
    }

    static void paintArrow(QPainter &pnt, const QRectF &face, uint32_t keysym, const QColor &ink)
    {
        const QPointF mid = face.center();
        const qreal size = face.height() * 0.22;
        QPolygonF head;
        switch (keysym) {
        case APPLE2SESSION_KEYSYM_LEFT: head << mid + QPointF(-size, 0) << mid + QPointF(size * 0.8, -size) << mid + QPointF(size * 0.8, size); break;
        case APPLE2SESSION_KEYSYM_RIGHT: head << mid + QPointF(size, 0) << mid + QPointF(-size * 0.8, -size) << mid + QPointF(-size * 0.8, size); break;
        case APPLE2SESSION_KEYSYM_UP: head << mid + QPointF(0, -size) << mid + QPointF(-size, size * 0.8) << mid + QPointF(size, size * 0.8); break;
        default: head << mid + QPointF(0, size) << mid + QPointF(-size, -size * 0.8) << mid + QPointF(size, -size * 0.8); break;
        }
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(ink);
        pnt.drawPolygon(head);
    }

    apple2session *m_session;
    const apple2_keycap *m_caps = nullptr;
    int m_count = 0;
    std::vector<int> m_state;
    int m_down = -1;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(apple2session *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the machine's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, int stolen);
    void pollCapture();
    void pollSlow();
    void setMode(int mode);
    void setModel(int model);
    void syncControls();
    void setKeyboardShown(bool shown);
    void rebuildPads();
    void refreshPadPorts();

    apple2session *m_session;
    StageWidget *m_stage = nullptr;
    KeyboardWidget *m_keyboard = nullptr;
    Segment *m_modeSeg = nullptr;
    Segment *m_modelSeg = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays the joystick" */
    unsigned m_padGen = ~0u;
    int m_padMode = -1;                   /* the mode the rows were built for */
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_debugButton = nullptr;
    QCheckBox *m_keyboardBox = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button (an axis, for the knob) for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(apple2session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("apple2HelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_stage = new StageWidget(
        session,
        [this](int target) { setMapState(target); },
        [this](const QString &text) { m_hint->setText(text); });
    m_stage->setObjectName(QStringLiteral("stage"));
    root->addWidget(m_stage, 0, Qt::AlignHCenter);

    /* what is plugged in, and which joystick it is */
    auto *plug = new QHBoxLayout;
    plug->addWidget(new QLabel(QStringLiteral("Plugged in:")));
    m_modeSeg = new Segment({ QStringLiteral("Joystick"), QStringLiteral("Paddles") }, QStringLiteral("mode"),
                            [this](int mode) { setMode(mode); });
    plug->addWidget(m_modeSeg);
    plug->addSpacing(14);
    plug->addWidget(new QLabel(QStringLiteral("Colour:")));
    m_modelSeg = new Segment({ QStringLiteral("Beige (IIe)"), QStringLiteral("Platinum (IIc)") }, QStringLiteral("model"),
                             [this](int model) { setModel(model); });
    plug->addWidget(m_modelSeg);
    plug->addStretch(1);
    root->addLayout(plug);

    /* the keyboard, when it is shown */
    m_keyboard = new KeyboardWidget(session);
    m_keyboard->setObjectName(QStringLiteral("keyboard"));
    root->addWidget(m_keyboard, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        bindings_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_stage->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        bindings_reset_defaults(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    /* Stop in Debugger has no part on the window: it is a key or a gamepad button only */
    m_debugButton = new QPushButton(QStringLiteral("Debugger key"));
    m_debugButton->setFocusPolicy(Qt::NoFocus);
    m_debugButton->setToolTip(QStringLiteral("Map a key or a gamepad button that stops the machine in the Debugger"));
    connect(m_debugButton, &QPushButton::clicked, this,
            [this] { setMapState(APPLE2_TARGET_SYSACT(APPLE2SESSION_SYSACT_DEBUG_STOP)); });
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(m_debugButton);
    mapRow->addStretch(1);
    mapRow->addWidget(apple2HelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    auto *optRow = new QHBoxLayout;
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    bindings->setChecked(apple2session_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        apple2session_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_stage->setShowBindings(checked);
    });
    m_stage->setShowBindings(bindings->isChecked());
    m_keyboardBox = new QCheckBox(QStringLiteral("Keyboard"));
    m_keyboardBox->setObjectName(QStringLiteral("keyboardBox"));
    m_keyboardBox->setFocusPolicy(Qt::NoFocus);
    m_keyboardBox->setToolTip(QStringLiteral("Show the Apple //e keyboard: click a key to type it"));
    m_keyboardBox->setChecked(apple2session_get_int(session, "controllers_keyboard", 0) > 0);
    connect(m_keyboardBox, &QCheckBox::toggled, this, [this](bool checked) { setKeyboardShown(checked); });
    optRow->addWidget(bindings);
    optRow->addWidget(m_keyboardBox);
    optRow->addStretch(1);
    root->addLayout(optRow);

    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    m_keyboard->setVisible(m_keyboardBox->isChecked());
    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] {
        m_stage->pollHeld();
        if (m_keyboard->isVisible()) m_keyboard->poll();
    });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    syncControls();
    letGo();
    m_stage->pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_stage->releaseAll();
    m_keyboard->releaseAll();
}

/* ---- the plugged-in controller ------------------------------------------------------- */

/* A different controller: whatever was held goes (keys, the mouse, the pads, the
 * knobs are centred), and the pad rows offer its ports. */
void ControllersWindow::setMode(int mode)
{
    if (apple2session_controller_mode(m_session) == mode) return;
    letGo();
    setMapState(-2);
    apple2session_set_controller_mode(m_session, mode);
    syncControls();
    rebuildPads();
}

void ControllersWindow::setModel(int model)
{
    apple2session_set_controller_model(m_session, model);
    syncControls();
}

/* The segments and the stage follow the session (a change made in Preferences). */
void ControllersWindow::syncControls()
{
    const int mode = apple2session_controller_mode(m_session);
    const int model = apple2session_controller_model(m_session);
    if (m_modeSeg->current() != mode) m_modeSeg->setCurrent(mode);
    if (m_modelSeg->current() != model) m_modelSeg->setCurrent(model);
    m_stage->syncState();
}

void ControllersWindow::setKeyboardShown(bool shown)
{
    apple2session_set_int(m_session, "controllers_keyboard", shown ? 1 : 0);
    if (!shown) m_keyboard->releaseAll();
    m_keyboard->setVisible(shown);
    layout()->activate();
    adjustSize();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    apple2CtlSetAccent(m_mapButton, state != -2);
    m_clearButton->setEnabled(state >= 0);
    m_debugButton->setEnabled(state != -2);
    if (state == -2) {
        m_captureTimer.stop();
        apple2session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_hint->setText(QStringLiteral("Press a part with the mouse to hold it. Drag the stick, or turn a knob. "
                                       "Alt is the Open Apple key."));
    } else if (state == -1) {
        m_captureTimer.stop();
        apple2session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_hint->setText(QStringLiteral("Click a part to remap it (the stick: a side of its well; a knob: its left "
                                       "half, right half or hub). Escape leaves Map."));
    } else {
        letGo();
        if (isKnobAxisTarget(state)) {
            m_hint->setText(QStringLiteral("Move a gamepad stick or trigger for %1 (Escape cancels).")
                                .arg(local(bindings_target_label(state))));
            apple2session_gamepad_capture_begin_axis(m_session);
        } else {
            m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                                .arg(local(bindings_target_label(state))));
            apple2session_gamepad_capture_begin(m_session);
        }
        m_captureTimer.start(50);
    }
    m_stage->setMap(m_mapState);
    m_stage->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, int stolen)
{
    setMapState(-1);
    m_hint->setText(stolen >= 0 ? QStringLiteral("%1 is now %2 (taken from %3)")
                                      .arg(local(bindings_target_label(target)), what,
                                           local(bindings_target_label(stolen)))
                                : QStringLiteral("%1 is now %2").arg(local(bindings_target_label(target)), what));
}

void ControllersWindow::pollCapture()
{
    int id;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!apple2session_gamepad_capture_poll(m_session, &id)) return;
    char name[48] = "";
    int stolen = -1;
    const int target = m_mapState;
    bindings_set_pad(m_session, target, id, &stolen);
    apple2session_pad_input_name(id, name, sizeof name);
    bound(target, local(name), stolen);
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads, auto-assignment changes, a mode changed in Preferences. */
void ControllersWindow::pollSlow()
{
    syncControls();
    const unsigned gen = apple2session_gamepad_generation(m_session);
    if (gen != m_padGen || apple2session_controller_mode(m_session) != m_padMode) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

namespace {
QString portName(int mode, int port)
{
    if (port < 0) return QStringLiteral("plays nothing");
    return mode == APPLE2_MODE_PADDLES ? QStringLiteral("plays paddle %1").arg(port)
                                       : QStringLiteral("plays the joystick");
}
} // namespace

void ControllersWindow::refreshPadPorts()
{
    const int mode = apple2session_controller_mode(m_session);
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const QString text = portName(mode, apple2session_gamepad_effective_port(m_session, idx));
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the port it plays and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    m_padMode = apple2session_controller_mode(m_session);
    const int count = apple2session_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    const int ports = apple2session_controller_ports(m_session);
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        apple2session_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic, 0 = the joystick / Paddle 0, 1 = Paddle 1 */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItem(QStringLiteral("Automatic"));
        if (ports == 1) {
            box->addItem(QStringLiteral("Joystick"));
        } else {
            box->addItem(QStringLiteral("Paddle 0"));
            box->addItem(QStringLiteral("Paddle 1"));
        }
        const int assign = apple2session_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign >= ports ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            apple2session_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPorts();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences ▸ Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    }
    refreshPadPorts();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = apple2session_gamepad_generation(m_session);
    syncControls();
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) {
        letGo();
        syncControls();
        m_stage->pollHeld();
    }
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_stage->pollHeld();
    m_heldTimer.start(25);
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(evt);
}

/* Only what this window holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        apple2session_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    if (evt->key() == Qt::Key_F9) {
        if (!evt->isAutoRepeat()) hide();
        return;
    }
    if (evt->key() == Qt::Key_F12) {
        if (!evt->isAutoRepeat()) DebuggerWindow::toggleFor(parentWidget(), m_session);
        return;
    }
    if (keyforward::isAppKey(evt)) return;      /* F10 and F11 belong to the main window */
    const uint32_t keysym = keyforward::physicalKeysym(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        if (isKnobAxisTarget(m_mapState)) {
            m_hint->setText(QStringLiteral("The knob follows a gamepad axis: move a stick or trigger."));
            return;
        }
        if (keysym >= APPLE2SESSION_KEYSYM_LMETA && keysym <= APPLE2SESSION_KEYSYM_RSUPER) {
            m_hint->setText(QStringLiteral("Alt, Meta and Super are the Open and Closed Apple keys: they cannot be mapped."));
            return;
        }
        char name[48] = "";
        int stolen = -1;
        const int target = m_mapState;
        bindings_set_keysym(m_session, target, keysym, &stolen);
        if (bindings_target_keysym(target) != keysym) {
            m_hint->setText(QStringLiteral("That key cannot be mapped."));
            return;
        }
        apple2session_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), stolen);
        return;
    }
    /* Otherwise what the main window does with it: the session's bindings
     * first (a console action goes to the main window's queue, as a gamepad's
     * does), then the machine; Alt is Open Apple here too. */
    keyforward::keyPress(m_session, evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = keyforward::physicalKeysym(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (keyforward::isAppKey(evt)) return;
    keyforward::keyRelease(m_session, evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void apple2ControllersToggle(QWidget *parent, apple2session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void apple2ControllersShow(QWidget *parent, apple2session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (!g_window->isVisible()) g_window->present();
    else g_window->raise();
}

bool apple2ControllersVisible()
{
    return g_window && g_window->isVisible();
}

bool apple2ControllersActive()
{
    return g_window && g_window->isActiveWindow();
}

void apple2ControllersRefresh()
{
    if (g_window) g_window->refresh();
}
