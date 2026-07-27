/*
 * FujiNet configuration (embedded web UI) and console log windows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "apple2session.h"

G_BEGIN_DECLS

/* Web UI window (WebKitGTK when built with it, otherwise opens the system
 * browser) and the FujiNet console log window. */
void apple2_fujinet_config_show(GtkWindow *parent, apple2session *session);
void apple2_fujinet_log_show(GtkWindow *parent, apple2session *session);

G_END_DECLS
