/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display. The frame is 560x384 or 640x400 (a VidHD card in slot 3
 * switches it), so the size comes with every frame.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>
#include <QtGlobal>

#include "DisplayView.h"

inline QRectF displayRect(QSizeF avail, int fbW, int fbH, int mode)
{
    const qreal w = avail.width(), h = avail.height();
    qreal dw, dh;

    if (mode == DisplayView::AspectInteger) {
        int scale = (int)qMin(w / fbW, h / fbH);
        if (scale < 1) scale = 1;
        dw = scale * fbW;
        dh = scale * fbH;
    } else {
        const qreal aspect = mode == DisplayView::AspectTv43
                                 ? 4.0 / 3.0
                                 : (qreal)fbW / fbH;
        if (w / h > aspect) {
            dh = h;
            dw = h * aspect;
        } else {
            dw = w;
            dh = w / aspect;
        }
    }
    return QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}
