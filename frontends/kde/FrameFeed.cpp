/*
 * FrameFeed -- see FrameFeed.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "FrameFeed.h"

#include <QMetaObject>

FrameFeed::FrameFeed(apple2session *session, QObject *parent)
    : QObject(parent), m_session(session)
{
}

FrameFeed::~FrameFeed()
{
    setActive(false);
}

/* Machine thread. Posting an event is the whole job. */
void FrameFeed::onFrame(void *self)
{
    auto *f = static_cast<FrameFeed *>(self);
    if (!f->m_pending.exchange(true, std::memory_order_acq_rel))
        QMetaObject::invokeMethod(f, &FrameFeed::deliver, Qt::QueuedConnection);
}

void FrameFeed::deliver()
{
    m_pending.store(false, std::memory_order_release);
    if (m_active)
        emit frameReady();
}

void FrameFeed::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active) {
        apple2session_set_frame_callback(m_session, &FrameFeed::onFrame, this);
        emit frameReady();   /* whatever arrived while nobody was looking */
    } else {
        /* returns after any call in flight, so `this` is safe to free after */
        apple2session_set_frame_callback(m_session, nullptr, nullptr);
    }
}
