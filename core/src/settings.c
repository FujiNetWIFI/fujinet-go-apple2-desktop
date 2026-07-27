/*
 * apple2session settings store: one flat key=value file shared by every
 * frontend (GTK, Qt, and later platforms), so a palette chosen in one app
 * is what the next one launches with. Deliberately not GSettings/KConfig.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

static setting_kv *find(apple2session *s, const char *key)
{
    setting_kv *kv;
    for (kv = s->settings; kv; kv = kv->next)
        if (strcmp(kv->key, key) == 0)
            return kv;
    return NULL;
}

static void set_locked(apple2session *s, const char *key, const char *val)
{
    setting_kv *kv = find(s, key);
    if (kv) {
        if (strcmp(kv->val, val) != 0) {
            free(kv->val);
            kv->val = strdup(val);
            s->settings_dirty = 1;
        }
        return;
    }
    kv = calloc(1, sizeof(*kv));
    if (!kv) return;
    kv->key = strdup(key);
    kv->val = strdup(val);
    kv->next = s->settings;
    s->settings = kv;
    s->settings_dirty = 1;
}

void settings_init(apple2session *s)
{
    FILE *fp;
    char line[1024];

    pthread_mutex_init(&s->settings_mtx, NULL);
    fp = fopen(s->settings_file, "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        char *eq, *end;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        end = eq + 1 + strcspn(eq + 1, "\r\n");
        *end = '\0';
        set_locked(s, p, eq + 1);
    }
    fclose(fp);
    s->settings_dirty = 0;
}

void settings_free_all(apple2session *s)
{
    setting_kv *kv = s->settings;
    while (kv) {
        setting_kv *next = kv->next;
        free(kv->key);
        free(kv->val);
        free(kv);
        kv = next;
    }
    s->settings = NULL;
    pthread_mutex_destroy(&s->settings_mtx);
}

const char *apple2session_get_str(apple2session *s, const char *key, const char *def)
{
    setting_kv *kv;
    const char *v;
    pthread_mutex_lock(&s->settings_mtx);
    kv = find(s, key);
    /* Values are only replaced, never freed, until apple2session_free; the
     * returned pointer stays valid for the session even after a set. */
    v = kv ? kv->val : def;
    pthread_mutex_unlock(&s->settings_mtx);
    return v;
}

void apple2session_set_str(apple2session *s, const char *key, const char *value)
{
    pthread_mutex_lock(&s->settings_mtx);
    set_locked(s, key, value ? value : "");
    pthread_mutex_unlock(&s->settings_mtx);
    apple2session_settings_flush(s);
}

int apple2session_get_int(apple2session *s, const char *key, int def)
{
    const char *v = apple2session_get_str(s, key, NULL);
    if (!v || !*v) return def;
    return atoi(v);
}

void apple2session_set_int(apple2session *s, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    apple2session_set_str(s, key, buf);
}

void apple2session_settings_flush(apple2session *s)
{
    char tmp[APPLE2_PATH_MAX + 8];
    FILE *fp;
    setting_kv *kv;

    pthread_mutex_lock(&s->settings_mtx);
    if (!s->settings_dirty) {
        pthread_mutex_unlock(&s->settings_mtx);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->settings_file);
    fp = fopen(tmp, "w");
    if (fp) {
        fprintf(fp, "# FujiNet Go Apple II shared settings\n");
        for (kv = s->settings; kv; kv = kv->next)
            fprintf(fp, "%s=%s\n", kv->key, kv->val);
        fclose(fp);
#if defined(_WIN32)
        /* Windows rename() will not replace an existing file. */
        remove(s->settings_file);
#endif
        rename(tmp, s->settings_file);
        s->settings_dirty = 0;
    }
    pthread_mutex_unlock(&s->settings_mtx);
}
