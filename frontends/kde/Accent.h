/*
 * The app's accent colour (APPLE2SESSION_ACCENT_RGB, the debugger's red).
 * Buttons in the accent state -- Map while armed, the selected side of a
 * segment -- carry the dynamic property "accent" and wear it with the accent's
 * ink; main.cpp's style sheet draws them. Held parts in the Controllers window
 * use it too.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPushButton>
#include <QStyle>

#include "apple2session.h"

/* the dark ink that reads on the red */
#define APPLE2_CTL_ACCENT_INK_RGB 0x1F0605u

inline QColor apple2CtlAccent()
{
    return QColor((APPLE2SESSION_ACCENT_RGB >> 16) & 0xff,
                  (APPLE2SESSION_ACCENT_RGB >> 8) & 0xff,
                  APPLE2SESSION_ACCENT_RGB & 0xff);
}

/* The accent's edge (a held part's outline shade) and the ink on it. */
inline QColor apple2CtlAccentDark() { return apple2CtlAccent().darker(140); }
inline QColor apple2CtlAccentInk()
{
    return QColor((APPLE2_CTL_ACCENT_INK_RGB >> 16) & 0xff,
                  (APPLE2_CTL_ACCENT_INK_RGB >> 8) & 0xff,
                  APPLE2_CTL_ACCENT_INK_RGB & 0xff);
}

/* Puts a button in or out of the accent state and repolishes it, so the style
 * sheet's [accent="true"] rule takes effect at once. */
inline void apple2CtlSetAccent(QPushButton *b, bool on)
{
    if (b->property("accent").toBool() == on) return;
    b->setProperty("accent", on);
    b->style()->unpolish(b);
    b->style()->polish(b);
}
