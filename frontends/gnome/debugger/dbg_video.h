/*
 * The debugger's Video tab (GTK4): a thin view over core/include/apple2vidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2_GNOME_DBG_VIDEO_H
#define APPLE2_GNOME_DBG_VIDEO_H

#include <gtk/gtk.h>

#include "apple2session.h"

/* "Show in Memory": jump the Memory tab to addr in bank (an apple2debug_bank). */
typedef void (*DbgVideoMemoryFn)(int bank, int addr, gpointer user);

/* The page. It owns its controller and frees it when destroyed. */
GtkWidget *dbg_video_new(apple2session *session, DbgVideoMemoryFn show_memory, gpointer user);

/* Bring the page up to date (only what changed is redrawn). The first call
 * after the page is mapped, or force, redraws everything. */
void dbg_video_refresh(GtkWidget *page, gboolean force);

/* Decoded view of a mode and page (Follow screen off), for the scripted captures. */
void dbg_video_select(GtkWidget *page, int mode, int page_no);

/* Pin the pixel at x,y of the (zoomed) picture, for the scripted captures. */
void dbg_video_pin(GtkWidget *page, int x, int y);

#endif
