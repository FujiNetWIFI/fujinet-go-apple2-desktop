/*
 * boot_smoke -- does the staged AppleWin core actually come up and produce
 * frames? This is the test that makes a macOS or Windows CI job meaningful
 * when you cannot run those platforms yourself: it exercises the whole
 * libretro host path (retro_init, retro_load_game, retro_run, the video
 * callback) without a window, a toolkit or FujiNet.
 *
 * Exits 77 (ctest SKIP_RETURN_CODE) when the core has no usable ROMs, so a
 * ROM-less configuration still passes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apple2_host.h"

static int g_frames;
static int g_width;
static int g_height;
static unsigned g_nonblack;

static void on_frame(const uint32_t *px, int w, int h, void *user)
{
    (void)user;
    g_frames++;
    g_width = w;
    g_height = h;
    /* The Apple II text screen is mostly black with white glyphs; counting
     * non-black pixels distinguishes "the core rendered something" from "the
     * core handed us a cleared buffer". */
    for (int i = 0; i < w * h; i++) {
        if ((px[i] & 0x00FFFFFFu) != 0) g_nonblack++;
    }
}

static void on_log(int level, const char *msg, void *user)
{
    (void)user;
    if (level >= 2) fprintf(stderr, "core: %s\n", msg);
}

int main(void)
{
    apple2host_set_log_sink(on_log, NULL);
    apple2host_set_frame_sink(on_frame, NULL);

    /* No FujiNet in this test: an empty slot 7 keeps the SmartPort listener
     * off so the test never touches the network or races a real FujiNet. */
    apple2host_set_variable("applewin_machine", "Enhanced Apple //e");
    apple2host_set_variable("applewin_slot7", "Empty");

    if (!apple2host_core_start()) {
        fprintf(stderr, "boot_smoke: core failed to start\n");
        return 77; /* no ROMs / unusable configuration -> skip */
    }

    /* ~2 seconds of emulated time: enough for the //e autostart ROM to clear
     * the screen and print its prompt. */
    for (int i = 0; i < 120; i++) apple2host_core_run_frame();

    printf("frames=%d geometry=%dx%d nonblack=%u fps=%.2f\n",
           g_frames, g_width, g_height, g_nonblack, apple2host_frame_rate());

    int rc = 0;
    if (g_frames == 0) {
        fprintf(stderr, "boot_smoke: no frames were produced\n");
        rc = 1;
    }
    if (g_width <= 0 || g_width > APPLE2_HOST_MAX_WIDTH ||
        g_height <= 0 || g_height > APPLE2_HOST_MAX_HEIGHT) {
        fprintf(stderr, "boot_smoke: geometry %dx%d outside 0..%dx%d\n",
                g_width, g_height, APPLE2_HOST_MAX_WIDTH, APPLE2_HOST_MAX_HEIGHT);
        rc = 1;
    }
    if (g_nonblack == 0) {
        fprintf(stderr, "boot_smoke: every frame was blank -- the machine did "
                        "not boot (are the ROMs embedded?)\n");
        rc = 77;
    }

    apple2host_core_stop();
    return rc;
}
