/*
 * What the main window needs from its display, so the GPU display and the
 * software fallback are interchangeable.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

class QWidget;

class DisplayView {
public:
    /* Same order as the GNOME frontend's Apple2AspectMode -- the mode is
     * stored in the shared settings, so the two must agree. */
    enum AspectMode {
        AspectTv43 = 0,
        AspectSquarePixels = 1,
        AspectInteger = 2,
    };

    virtual ~DisplayView() = default;
    virtual QWidget *widget() = 0;
    virtual void setAspectMode(int mode) = 0;
    virtual void setSmooth(bool smooth) = 0;
};
