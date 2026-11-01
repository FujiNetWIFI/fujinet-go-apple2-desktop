/*
 * FrameFeed -- tells the GUI thread when the session has a new frame.
 *
 * Qt Widgets has no frame clock to hang a tick on, so a display would poll
 * the session from a timer, forever, to move one
 * frame in 60. The session now calls back from the machine thread after
 * each frame; this turns that into a queued signal on the GUI thread.
 *
 *  - Coalescing: at most one signal is in flight, so a GUI thread that falls
 *    behind sees one wakeup, not a backlog; the frame it copies is the
 *    newest anyway (the slot is latest-wins).
 *  - Idle: while inactive (the window is hidden or minimised) the callback
 *    is unregistered altogether, so a minimised emulator costs the GUI
 *    thread no wakeups. Becoming active again signals once, to catch up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <atomic>

#include "apple2session.h"

class FrameFeed : public QObject {
    Q_OBJECT
public:
    explicit FrameFeed(apple2session *session, QObject *parent = nullptr);
    ~FrameFeed() override;

    void setActive(bool active);

signals:
    void frameReady();

private:
    static void onFrame(void *self);
    void deliver();

    apple2session *m_session;
    std::atomic<bool> m_pending{false};
    bool m_active = false;
};
