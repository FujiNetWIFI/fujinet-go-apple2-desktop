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

struct _Apple2Display {
    GtkWidget parent_instance;

    apple2session *session;
    GdkTexture *texture;
    /* Heap-allocated: a GObject instance must stay under 64K, and this is
     * ~1MB at the largest geometry. */
    uint32_t *fb;
    int fb_w;
    int fb_h;
    uint64_t serial;
    guint tick_id;
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

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    Apple2Display *self = APPLE2_DISPLAY(widget);
    int w = 0, h = 0;
    (void)user_data;

    if (!self->session)
        return G_SOURCE_CONTINUE;

    /* Frame-clock time is CLOCK_MONOTONIC microseconds. */
    apple2session_notify_vsync(self->session,
                               gdk_frame_clock_get_frame_time(clock) * 1000);

    if (apple2session_copy_frame(self->session, self->fb, &self->serial, &w, &h)
        && w > 0 && h > 0) {
        GBytes *bytes = g_bytes_new(self->fb,
                                    (gsize)w * (gsize)h * sizeof(uint32_t));
        self->fb_w = w;
        self->fb_h = h;
        g_clear_object(&self->texture);
        self->texture = gdk_memory_texture_new(w, h, APPLE2_GDK_FORMAT, bytes,
                                               (gsize)w * sizeof(uint32_t));
        g_bytes_unref(bytes);
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
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
    g_clear_pointer(&self->fb, g_free);
    G_OBJECT_CLASS(apple2_display_parent_class)->dispose(object);
}

static void apple2_display_class_init(Apple2DisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = apple2_display_snapshot;
    object_class->dispose = apple2_display_dispose;
}

static void apple2_display_init(Apple2Display *self)
{
    self->fb = g_malloc0((gsize)APPLE2SESSION_FB_MAX_WIDTH *
                         APPLE2SESSION_FB_MAX_HEIGHT * sizeof(uint32_t));
    self->aspect_mode = APPLE2_ASPECT_TV_4_3;
    self->smooth = FALSE;
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    self->tick_id =
        gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb, NULL, NULL);
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
