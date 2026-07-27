/*
 * DisplayWidget: the emulator video widget for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QImage>
#include <QOpenGLWidget>

#include <vector>

#include "apple2session.h"

class DisplayWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    /* Same order as the GNOME frontend's Apple2AspectMode -- the mode is
     * stored in the shared settings, so the two must agree. */
    enum AspectMode {
        AspectTv43 = 0,
        AspectSquarePixels = 1,
        AspectInteger = 2,
    };

    explicit DisplayWidget(apple2session *session, QWidget *parent = nullptr);

    void setAspectMode(int mode);
    void setSmooth(bool smooth);

protected:
    void paintGL() override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void onFrameSwapped();
    void forwardKey(const QKeyEvent *event, int down);
    QRectF destRect() const;

    apple2session *m_session;
    /* The session writes tightly-packed XRGB8888 rows, so the frame is kept
     * in a plain buffer and QImage is only ever a view over it -- QImage's
     * own stride is an implementation detail we must not depend on. On a
     * little-endian host XRGB8888 is exactly QImage::Format_RGB32.
     * Sized for the largest geometry the core can produce, because a VidHD
     * card in slot 3 switches it between 560x384 and 640x400 at runtime. */
    std::vector<uint32_t> m_frame;
    int m_fbW = 0;
    int m_fbH = 0;
    /* uint64_t, not quint64: the session takes a uint64_t* and the two are
     * distinct types here (unsigned long vs unsigned long long). */
    uint64_t m_serial = 0;
    int m_aspectMode = AspectTv43;
    bool m_smooth = false;
};
