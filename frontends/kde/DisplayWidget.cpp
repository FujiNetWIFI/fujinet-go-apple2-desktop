/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include "DisplayGeometry.h"
#include "KeyForward.h"

#include <QEvent>
#include <QFile>
#include <QMatrix4x4>
#include <QVector4D>
#include <QWindow>

static QShader loadShader(const char *path)
{
    QFile f(QString::fromLatin1(path));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

DisplayWidget::DisplayWidget(apple2session *session, QWidget *parent)
    : QRhiWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_frame((size_t)APPLE2SESSION_FB_MAX_WIDTH * APPLE2SESSION_FB_MAX_HEIGHT, 0)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(APPLE2SESSION_FB_MAX_WIDTH / 2, APPLE2SESSION_FB_MAX_HEIGHT / 2);
    connect(m_feed, &FrameFeed::frameReady, this, &DisplayWidget::onFrame);
}

DisplayWidget::~DisplayWidget()
{
    m_feed->setActive(false);
}

void DisplayWidget::setAspectMode(int mode) { m_aspectMode = mode; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

/* ---- idle ------------------------------------------------------------------
 * A hidden or minimised window has nobody to draw for: the feed unregisters
 * from the session, so the GUI thread gets no wakeups at all until it is
 * shown again. The machine itself keeps running (netplay must not stall). */

void DisplayWidget::updateActive()
{
    QWidget *w = window();
    m_feed->setActive(isVisible() && w && !w->isMinimized());
}

void DisplayWidget::showEvent(QShowEvent *e)
{
    QRhiWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void DisplayWidget::hideEvent(QHideEvent *e)
{
    QRhiWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool DisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QRhiWidget::eventFilter(watched, e);
}

bool DisplayWidget::event(QEvent *e)
{
    if (keyforward::shortcutOverride(e))
        return true;
    /* The compositor can mark a window occluded without minimising it */
    if (e->type() == QEvent::ScreenChangeInternal || e->type() == QEvent::WindowActivate)
        updateActive();
    return QRhiWidget::event(e);
}

void DisplayWidget::keyPressEvent(QKeyEvent *event) { keyforward::keyPress(m_session, event); }
void DisplayWidget::keyReleaseEvent(QKeyEvent *event) { keyforward::keyRelease(m_session, event); }

/* ---- frames ---------------------------------------------------------------- */

void DisplayWidget::onFrame()
{
    int w = 0, h = 0;
    /* straight into the buffer: one copy, and no alpha pass -- the shader
     * ignores the X byte */
    if (!apple2session_copy_frame(m_session, m_frame.data(), &m_serial, &w, &h))
        return;
    if (w <= 0 || h <= 0)
        return;
    m_fbW = w;
    m_fbH = h;
    m_dirty = true;
    update();
}

/* ---- GPU ------------------------------------------------------------------- */

void DisplayWidget::releaseResources()
{
    m_pipeline.reset();
    m_srb.reset();
    m_linear.reset();
    m_nearest.reset();
    m_tex.reset();
    m_ubuf.reset();
    m_texW = m_texH = 0;
}

void DisplayWidget::rebuildTexture(int w, int h)
{
    m_tex.reset(m_rhi->newTexture(QRhiTexture::BGRA8, QSize(w, h)));
    m_tex->create();
    m_texW = w;
    m_texH = h;
    m_srb.reset();   /* it names the old texture */
    /* A new texture is empty. A still picture publishes nothing more, so
     * re-upload what the frame buffer already holds (after a lost device or
     * an RHI change the old texture's content is gone). */
    if (m_fbW > 0)
        m_dirty = true;
}

void DisplayWidget::initialize(QRhiCommandBuffer *)
{
    if (m_rhi != rhi()) {
        releaseResources();
        m_rhi = rhi();
    }
    if (m_pipeline)
        return;

    m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    m_ubuf->create();
    m_nearest.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                      QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    m_nearest->create();
    m_linear.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                     QRhiSampler::None, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge));
    m_linear->create();
    if (!m_tex)
        rebuildTexture(m_fbW > 0 ? m_fbW : 560, m_fbH > 0 ? m_fbH : 384);

    m_smoothApplied = m_smooth;
    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_tex.get(), m_smooth ? m_linear.get() : m_nearest.get()),
    });
    m_srb->create();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, loadShader(":/shaders/display.vert.qsb") },
        { QRhiShaderStage::Fragment, loadShader(":/shaders/display.frag.qsb") },
    });
    m_pipeline->setVertexInputLayout({});
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!m_pipeline->create()) {
        m_pipeline.reset();
        emit renderFailed();
    }
}

void DisplayWidget::render(QRhiCommandBuffer *cb)
{
    if (!m_pipeline)
        return;
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();

    /* A VidHD switch between 560x384 and 640x400 changes the frame's size:
     * the texture is exactly the frame's size so the sampler never blends in
     * a stale line, and the bindings are rebuilt around it. */
    bool rebind = false;
    if (m_fbW > 0 && (m_fbW != m_texW || m_fbH != m_texH)) {
        rebuildTexture(m_fbW, m_fbH);
        rebind = true;
    }
    if (m_smooth != m_smoothApplied) {
        m_smoothApplied = m_smooth;
        rebind = true;
    }
    if (rebind || !m_srb) {
        m_srb.reset(m_rhi->newShaderResourceBindings());
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      m_tex.get(), m_smooth ? m_linear.get() : m_nearest.get()),
        });
        m_srb->create();
    }

    if (m_dirty && m_fbW > 0) {
        QRhiTextureSubresourceUploadDescription sub;
        const qsizetype bytes = (qsizetype)m_fbW * m_fbH * 4;
        sub.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(m_frame.data()), bytes));
        sub.setDataStride(m_fbW * 4);
        sub.setSourceSize(QSize(m_fbW, m_fbH));
        u->uploadTexture(m_tex.get(), QRhiTextureUploadDescription({ QRhiTextureUploadEntry(0, 0, sub) }));
        m_dirty = false;
    }

    const QSize px = renderTarget()->pixelSize();
    const QRectF r = displayRect(QSizeF(px), m_fbW > 0 ? m_fbW : m_texW,
                                 m_fbW > 0 ? m_fbH : m_texH, m_aspectMode);

    /* pixel rect -> clip space (y up), then through the backend's correction
     * so Vulkan's flipped y comes out the right way up */
    auto clip = [&](double x, double y) {
        return m_rhi->clipSpaceCorrMatrix() *
               QVector4D(float(x / px.width() * 2.0 - 1.0), float(1.0 - y / px.height() * 2.0), 0.f, 1.f);
    };
    const QVector4D tl = clip(r.left(), r.top());
    const QVector4D br = clip(r.right(), r.bottom());
    const float rect[4] = { tl.x(), tl.y(), br.x(), br.y() };
    u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof rect, rect);

    cb->beginPass(renderTarget(), Qt::black, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
    cb->setShaderResources(m_srb.get());
    cb->draw(4);
    cb->endPass();
}
