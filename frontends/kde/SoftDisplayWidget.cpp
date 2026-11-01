/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"

#include <QEvent>
#include <QPainter>

#include "KeyForward.h"

SoftDisplayWidget::SoftDisplayWidget(apple2session *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_frame((size_t)APPLE2SESSION_FB_MAX_WIDTH * APPLE2SESSION_FB_MAX_HEIGHT, 0)
{
    setMinimumSize(APPLE2SESSION_FB_MAX_WIDTH / 2, APPLE2SESSION_FB_MAX_HEIGHT / 2);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::setAspectMode(int mode) { m_aspectMode = mode; update(); }
void SoftDisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

bool SoftDisplayWidget::event(QEvent *e)
{
    if (keyforward::shortcutOverride(e))
        return true;
    return QWidget::event(e);
}

void SoftDisplayWidget::keyPressEvent(QKeyEvent *event) { keyforward::keyPress(m_session, event); }
void SoftDisplayWidget::keyReleaseEvent(QKeyEvent *event) { keyforward::keyRelease(m_session, event); }

void SoftDisplayWidget::onFrame()
{
    int w = 0, h = 0;
    if (!apple2session_copy_frame(m_session, m_frame.data(), &m_serial, &w, &h))
        return;
    if (w <= 0 || h <= 0)
        return;
    m_fbW = w;
    m_fbH = h;
    update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_fbW <= 0 || m_fbH <= 0) return;

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    /* A view over the packed buffer; Format_RGB32 ignores the X byte. */
    const QImage frame(reinterpret_cast<const uchar *>(m_frame.data()), m_fbW, m_fbH,
                       m_fbW * 4, QImage::Format_RGB32);
    p.drawImage(displayRect(QSizeF(size()), m_fbW, m_fbH, m_aspectMode), frame);
}
