/*
 * KeyForward -- see KeyForward.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

namespace keyforward {

namespace {

/* Qt key code -> X11/xkb keysym, for the keys apple2_key_from_event resolves
 * by keycode rather than by character. */
quint32 keysymForKey(const QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:     return 0xFF0D;
    case Qt::Key_Escape:    return 0xFF1B;
    case Qt::Key_Tab:       return 0xFF09;
    case Qt::Key_Backspace: return 0xFF08;
    case Qt::Key_Delete:    return 0xFFFF;
    case Qt::Key_Left:      return 0xFF51;
    case Qt::Key_Up:        return 0xFF52;
    case Qt::Key_Right:     return 0xFF53;
    case Qt::Key_Down:      return 0xFF54;
    /* The Apple keys. Qt reports left and right Alt as the same key code and
     * distinguishes them only by the KeypadModifier-style native flags, so
     * use the native scancode-independent path: Qt::Key_Alt is Open Apple and
     * Qt::Key_AltGr is the right-hand one (Closed Apple). Meta is accepted
     * too because a Mac keyboard's Command keys land there. */
    case Qt::Key_Alt:       return 0xFFE9; /* Alt_L  -> Open Apple */
    case Qt::Key_AltGr:     return 0xFFEA; /* Alt_R  -> Closed Apple */
    case Qt::Key_Meta:      return 0xFFE7; /* Meta_L -> Open Apple */
    default:                return 0;
    }
}

void forward(apple2session *session, const QKeyEvent *event, int down)
{
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    quint32 unicode = 0;
    if (!event->text().isEmpty())
        unicode = event->text().at(0).unicode();

    quint32 keysym = keysymForKey(event);
    if (keysym == 0) {
        /* Printable: Qt gives no text for a Ctrl combo, so fall back to the
         * unshifted letter, which is what the keysym contract wants. */
        if (ctrl && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
            keysym = 'a' + (quint32)(event->key() - Qt::Key_A);
        else
            keysym = unicode;
    }
    if (keysym == 0)
        return;

    apple2session_key(session, down, keysym, unicode, ctrl ? 1 : 0,
                      shift ? 1 : 0);
}


} // namespace

bool shortcutOverride(QEvent *event)
{
    if (event->type() != QEvent::ShortcutOverride)
        return false;
    QKeyEvent *ke = static_cast<QKeyEvent *>(event);
    if (ke->key() == Qt::Key_F10 || ke->key() == Qt::Key_F11 ||
        ke->key() == Qt::Key_F12)
        return false;
    event->accept();
    return true;
}

void keyPress(apple2session *session, QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        forward(session, event, 1);
    event->accept();
}

void keyRelease(apple2session *session, QKeyEvent *event)
{
    if (!event->isAutoRepeat())
        forward(session, event, 0);
    event->accept();
}

} // namespace keyforward
