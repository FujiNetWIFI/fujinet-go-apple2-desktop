/*
 * Apple2Display: paints the emulator's latest XRGB8888 frame with a
 * GdkMemoryTexture, letterboxed to the chosen aspect inside whatever size
 * the window manager gives us (tiling or floating). A frame-clock tick
 * callback doubles as the vsync source for the session's phase-lock:
 * GTK4's GPU renderer presents on the compositor's vsync, so pacing the
 * emulator on these ticks is pacing it on display vsync.
 *
 * The core's geometry is not fixed -- a VidHD card in slot 3 switches it
 * between 560x384 and 640x400 -- so the frame's dimensions come back with
 * every copy and the texture is rebuilt to match.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

/* ---- frame buffer pool ----------------------------------------------------
 * The session copies a frame straight into a pooled buffer and the texture
 * borrows that buffer: one copy per frame, no allocation in steady state
 * (a 1 MB malloc is an mmap/munmap pair every frame). The buffer comes
 * back to the pool when GSK drops the texture's bytes, possibly after the
 * widget is gone, so the pool is reference counted and each outstanding
 * buffer holds a reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle uint32 buffers, FB_MAX_WIDTH * FB_MAX_HEIGHT each */
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc((gsize)APPLE2SESSION_FB_MAX_WIDTH *
                              APPLE2SESSION_FB_MAX_HEIGHT * sizeof(guint32));
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _Apple2Display {
    GtkWidget parent_instance;

    apple2session *session;
    GdkTexture *texture;
    FramePool *pool;
    int fb_w;
    int fb_h;
    uint64_t serial;
    guint tick_id;
    gulong suspended_id;   /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
    Apple2AspectMode aspect_mode;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(Apple2Display, apple2_display, GTK_TYPE_WIDGET)

/* The core's XRGB8888 is a uint32 0x00RRGGBB, so in memory on a
 * little-endian host the bytes run B,G,R,X -- which is exactly
 * GDK_MEMORY_B8G8R8X8, and no per-pixel conversion is needed. On a
 * big-endian host the same uint32 lays out X,R,G,B. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define APPLE2_GDK_FORMAT GDK_MEMORY_X8R8G8B8
#else
#define APPLE2_GDK_FORMAT GDK_MEMORY_B8G8R8X8
#endif

/* Wrap px (w x h, tightly packed) in the texture. The previous texture, when
 * it was the same size (a VidHD switch changes it), goes in as the update
 * source so GSK can reuse its GPU texture. Takes ownership of px. */
static void publish_texture(Apple2Display *self, guint32 *px, int w, int h)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(px, (gsize)w * (gsize)h * sizeof(guint32),
                                       loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)w * sizeof(guint32));
    gdk_memory_texture_builder_set_width(b, w);
    gdk_memory_texture_builder_set_height(b, h);
    gdk_memory_texture_builder_set_format(b, APPLE2_GDK_FORMAT);
    if (self->texture && self->fb_w == w && self->fb_h == h) {
        cairo_rectangle_int_t all = { 0, 0, w, h };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    Apple2Display *self = APPLE2_DISPLAY(widget);
    guint32 *px;
    int w = 0, h = 0;
    (void)user_data;

    if (!self->session)
        return G_SOURCE_CONTINUE;

    /* Frame-clock time is CLOCK_MONOTONIC microseconds. */
    apple2session_notify_vsync(self->session,
                               gdk_frame_clock_get_frame_time(clock) * 1000);

    px = pool_take(self->pool);
    if (apple2session_copy_frame(self->session, px, &self->serial, &w, &h)
        && w > 0 && h > 0) {
        publish_texture(self, px, w, h);
        self->fb_w = w;
        self->fb_h = h;
        gtk_widget_queue_draw(widget);
    } else {
        pool_give(self->pool, px);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. A hidden or
 * suspended window gets no frame callbacks from the compositor anyway, but a
 * minimised one on some stacks keeps ticking, and every tick is a wakeup.
 * The emulator does not care: with no ticks the vsync stream goes stale
 * and the session paces on the wall clock. The first tick after the window
 * comes back copies whatever frame is newest. */
static void update_tick(Apple2Display *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb,
                                                     NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(APPLE2_DISPLAY(user_data));
}

static void apple2_display_map(GtkWidget *widget)
{
    Apple2Display *self = APPLE2_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(apple2_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void apple2_display_unmap(GtkWidget *widget)
{
    Apple2Display *self = APPLE2_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(apple2_display_parent_class)->unmap(widget);
    update_tick(self);
}

static void apple2_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    Apple2Display *self = APPLE2_DISPLAY(widget);
    const float w = (float)gtk_widget_get_width(widget);
    const float h = (float)gtk_widget_get_height(widget);
    float dw, dh, aspect;
    graphene_rect_t dest;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, 1},
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w < 1 || h < 1 || self->fb_w < 1 || self->fb_h < 1)
        return;

    aspect = self->aspect_mode == APPLE2_ASPECT_TV_4_3
                 ? 4.0f / 3.0f
                 : (float)self->fb_w / (float)self->fb_h;

    if (self->aspect_mode == APPLE2_ASPECT_INTEGER) {
        int scale = (int)MIN(w / self->fb_w, h / self->fb_h);
        if (scale < 1) scale = 1;
        dw = (float)(scale * self->fb_w);
        dh = (float)(scale * self->fb_h);
    } else if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }

    dest = GRAPHENE_RECT_INIT((w - dw) / 2.0f, (h - dh) / 2.0f, dw, dh);
    gtk_snapshot_append_scaled_texture(snapshot, self->texture,
                                       self->smooth ? GSK_SCALING_FILTER_LINEAR
                                                    : GSK_SCALING_FILTER_NEAREST,
                                       &dest);
}

static void apple2_display_dispose(GObject *object)
{
    Apple2Display *self = APPLE2_DISPLAY(object);
    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }
    G_OBJECT_CLASS(apple2_display_parent_class)->dispose(object);
}

static void apple2_display_class_init(Apple2DisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = apple2_display_snapshot;
    widget_class->map = apple2_display_map;
    widget_class->unmap = apple2_display_unmap;
    object_class->dispose = apple2_display_dispose;
}

static void apple2_display_init(Apple2Display *self)
{
    self->pool = pool_new();
    self->aspect_mode = APPLE2_ASPECT_TV_4_3;
    self->smooth = FALSE;
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
}

GtkWidget *apple2_display_new(apple2session *session)
{
    Apple2Display *self = g_object_new(APPLE2_TYPE_DISPLAY, NULL);
    self->session = session;
    return GTK_WIDGET(self);
}

void apple2_display_set_aspect_mode(Apple2Display *self, Apple2AspectMode mode)
{
    self->aspect_mode = mode;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void apple2_display_set_smooth(Apple2Display *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
