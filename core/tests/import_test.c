/*
 * Media import with a native path. On Windows the file dialog hands back
 * "C:\dir\name.dsk"; the import must keep only "name.dsk" in the FujiNet SD
 * folder, not graft the whole drive path onto it (issue #1).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define SEP "\\"
#else
#include <unistd.h>
#define SEP "/"
#endif

#include "apple2session.h"
#include "session_internal.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void make_dir(const char *path)
{
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    if (system(cmd)) failures++;
#endif
}

static void import_case(apple2session *s, const char *dir, const char *name)
{
    char src[1024], expect[1024], dest[1024];
    FILE *f;

    snprintf(src, sizeof(src), "%s%s%s", dir, SEP, name);
    f = fopen(src, "wb");
    if (!f) { fprintf(stderr, "FAIL cannot create %s\n", src); failures++; return; }
    fputs("disk", f);
    fclose(f);

    dest[0] = '\0';
    check(apple2session_import_media(s, src, dest, sizeof(dest)) == 0, name);
    snprintf(expect, sizeof(expect), "%s/%s", s->fujinet_sd, name);
    check(exists(expect), "imported file lands directly in the SD folder");
    if (failures)
        fprintf(stderr, "  src %s\n  dest %s\n  err %s\n", src, dest,
                apple2session_last_error(s));
}

int main(void)
{
    char root[900], cfg[1000], data[1000], src[1000];
    apple2session_paths p;
    apple2session *s;
    const char *tmp = getenv("TEMP");

    if (!tmp || !*tmp) tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(root, sizeof(root), "%s%simport_test_%d", tmp, SEP,
#ifdef _WIN32
             (int)GetCurrentProcessId()
#else
             (int)getpid()
#endif
    );
    make_dir(root);
    snprintf(cfg, sizeof(cfg), "%s%scfg", root, SEP);
    snprintf(data, sizeof(data), "%s%sdata", root, SEP);
    snprintf(src, sizeof(src), "%s%sfrom", root, SEP);
    make_dir(src);

    memset(&p, 0, sizeof(p));
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";
    s = apple2session_new(&p);
    if (!s) { fprintf(stderr, "FAIL session\n"); return 1; }
    paths_provision_fujinet(s);

    import_case(s, src, "plain.dsk");
    import_case(s, src, "Arkanoid (1988)(Taito America)(II+)[cr 4am][64K].dsk");

    apple2session_free(s);
    return failures ? 1 : 0;
}
