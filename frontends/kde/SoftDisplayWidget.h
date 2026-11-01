/*
 * The software display: the fallback for a machine with no usable GPU
 * device (or QT_QUICK_BACKEND=software style setups). A QWidget that scales
 * with QPainter, fed by FrameFeed like the GPU one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "apple2session.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(apple2session *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }
    void setAspectMode(int mode) override;
    void setSmooth(bool smooth) override;

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    apple2session *m_session;
    FrameFeed *m_feed;
    std::vector<uint32_t> m_frame;   /* straight from copy_frame, largest geometry */
    uint64_t m_serial = 0;
    int m_fbW = 0;
    int m_fbH = 0;
    int m_aspectMode = AspectTv43;
    bool m_smooth = false;
};
