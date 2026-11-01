/*
 * The emulator display on the GPU: a QRhiWidget that keeps one texture the
 * size of the frame (560x384 or 640x400, rebuilt when a VidHD card switches
 * it) and draws one textured quad, only when the session has published a new
 * frame (see FrameFeed). Qt picks Vulkan, Metal, D3D or OpenGL for it; the
 * scaling, aspect and filtering all happen on the GPU.
 *
 * If no GPU device can be had it emits renderFailed() and the main window
 * swaps in the software SoftDisplayWidget.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRhiWidget>
#include <memory>
#include <vector>
#include <rhi/qrhi.h>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "apple2session.h"

class DisplayWidget : public QRhiWidget, public DisplayView {
    Q_OBJECT
public:
    explicit DisplayWidget(apple2session *session, QWidget *parent = nullptr);
    ~DisplayWidget() override;

    QWidget *widget() override { return this; }
    void setAspectMode(int mode) override;
    void setSmooth(bool smooth) override;

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    /* Tab is a key like any other here: it reaches the key handler instead
     * of moving the focus. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();
    void rebuildTexture(int w, int h);

    apple2session *m_session;
    FrameFeed *m_feed;
    /* The session writes tightly-packed XRGB8888 rows straight in here (one
     * copy), sized for the largest geometry. */
    std::vector<uint32_t> m_frame;
    uint64_t m_serial = 0;
    int m_fbW = 0;             /* the newest frame's size */
    int m_fbH = 0;
    bool m_dirty = false;      /* m_frame holds a frame not yet uploaded */
    int m_aspectMode = AspectTv43;
    bool m_smooth = false;
    bool m_smoothApplied = false;

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_nearest, m_linear;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    int m_texW = 0;
    int m_texH = 0;
};
