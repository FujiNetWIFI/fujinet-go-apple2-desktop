/*
 * pad_assign -- the remembered pad -> port table (see pad_assign.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <string.h>

#include "pad_assign.h"

static int hex_value(int chr)
{
    if (chr >= '0' && chr <= '9') return chr - '0';
    if (chr >= 'a' && chr <= 'f') return chr - 'a' + 10;
    if (chr >= 'A' && chr <= 'F') return chr - 'A' + 10;
    return -1;
}

/* A GUID as lower-case hex into dst; 0 if empty, too long or not hex. */
static int guid_norm(char *dst, const char *src, size_t len)
{
    size_t idx;
    if (!src || len == 0 || len >= APPLE2_PAD_GUID_MAX) return 0;
    for (idx = 0; idx < len; idx++) {
        if (hex_value((unsigned char)src[idx]) < 0) return 0;
        dst[idx] = (char)tolower((unsigned char)src[idx]);
    }
    dst[len] = '\0';
    return 1;
}

static int needs_escape(unsigned char chr)
{
    return chr < 0x20 || chr == 0x7F || chr == ';' || chr == '|' || chr == '=' || chr == '%';
}

/* The exact entry for a normalised GUID and a name, or -1. */
static int find_exact(const apple2_pad_table *tab, const char *guid, const char *name)
{
    int idx;
    for (idx = tab->count - 1; idx >= 0; idx--)
        if (!strcmp(tab->entry[idx].guid, guid) && !strcmp(tab->entry[idx].name, name))
            return idx;
    return -1;
}

static void erase_at(apple2_pad_table *tab, int idx)
{
    memmove(&tab->entry[idx], &tab->entry[idx + 1],
            (size_t)(tab->count - idx - 1) * sizeof tab->entry[0]);
    tab->count--;
}

void apple2_pad_table_clear(apple2_pad_table *tab)
{
    memset(tab, 0, sizeof *tab);
}

int apple2_pad_table_find(const apple2_pad_table *tab, const char *guid, const char *name)
{
    char norm[APPLE2_PAD_GUID_MAX];
    int idx;
    if (!tab || !name) return -1;
    if (guid && guid_norm(norm, guid, strlen(guid)))
        for (idx = tab->count - 1; idx >= 0; idx--)
            if (!strcmp(tab->entry[idx].guid, norm)) return idx;
    for (idx = tab->count - 1; idx >= 0; idx--)
        if (!strcmp(tab->entry[idx].name, name)) return idx;
    return -1;
}

int apple2_pad_table_set(apple2_pad_table *tab, const char *guid, const char *name, int port)
{
    char norm[APPLE2_PAD_GUID_MAX];
    int idx;
    if (!tab || !guid || !name || port < 0 || strlen(name) >= APPLE2_PAD_NAME_MAX ||
        !guid_norm(norm, guid, strlen(guid)))
        return -1;
    idx = find_exact(tab, norm, name);
    if (idx >= 0) {
        apple2_pad_entry ent = tab->entry[idx];
        if (ent.port == port) return 0;
        ent.port = port;
        erase_at(tab, idx);      /* a changed entry is the newest */
        tab->entry[tab->count++] = ent;
        return 1;
    }
    if (tab->count >= APPLE2_PAD_ASSIGN_MAX) erase_at(tab, 0);
    idx = tab->count++;
    memcpy(tab->entry[idx].guid, norm, sizeof tab->entry[idx].guid);
    memcpy(tab->entry[idx].name, name, strlen(name) + 1);
    tab->entry[idx].port = port;
    return 1;
}

int apple2_pad_table_remove(apple2_pad_table *tab, const char *guid, const char *name,
                         int nkeep, const char *const *keepguid, const char *const *keepname)
{
    int removed = 0, idx, kix;
    if (!tab || !name) return 0;
    /* every entry that find() would hand this device, so that none is left
     * to apply it on the next run (a stale GUID of the same pad included) --
     * but never one that find() also hands a pad that keeps its assignment */
    while ((idx = apple2_pad_table_find(tab, guid, name)) >= 0) {
        for (kix = 0; kix < nkeep; kix++)
            if (apple2_pad_table_find(tab, keepguid[kix], keepname[kix]) == idx) return removed;
        erase_at(tab, idx);
        removed = 1;
    }
    return removed;
}

int apple2_pad_table_encode(const apple2_pad_table *tab, char *dst, size_t dstsz)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t len = 0;
    int idx;
    char tail[16];

    if (!tab || !dst || dstsz == 0) return -1;
#define PUT(chr) do { if (len + 1 >= dstsz) { dst[0] = '\0'; return -1; } dst[len++] = (char)(chr); } while (0)
    for (idx = 0; idx < tab->count; idx++) {
        const apple2_pad_entry *ent = &tab->entry[idx];
        const char *src;
        int num = ent->port, ndig = 0;

        if (idx) PUT(';');
        for (src = ent->guid; *src; src++) PUT(*src);
        PUT('|');
        for (src = ent->name; *src; src++) {
            unsigned char chr = (unsigned char)*src;
            if (needs_escape(chr)) {
                PUT('%');
                PUT(digits[chr >> 4]);
                PUT(digits[chr & 15]);
            } else {
                PUT(chr);
            }
        }
        PUT('=');
        do { tail[ndig++] = (char)('0' + num % 10); num /= 10; } while (num && ndig < (int)sizeof tail);
        while (ndig) PUT(tail[--ndig]);
    }
#undef PUT
    dst[len] = '\0';
    return (int)len;
}

/* One "<guid>|<name>=<port>" entry; 1 if it parsed. */
static int parse_entry(apple2_pad_entry *ent, const char *beg, const char *end, int nports)
{
    const char *bar, *eqpos, *src;
    size_t nlen = 0;
    long port = 0;

    for (bar = beg; bar < end && *bar != '|'; bar++) {}
    if (bar >= end) return 0;
    for (eqpos = bar + 1; eqpos < end && *eqpos != '='; eqpos++) {}
    if (eqpos >= end) return 0;
    if (!guid_norm(ent->guid, beg, (size_t)(bar - beg))) return 0;

    for (src = bar + 1; src < eqpos; src++) {
        unsigned char chr = (unsigned char)*src;
        if (nlen + 1 >= APPLE2_PAD_NAME_MAX || chr == '|') return 0;
        if (chr == '%') {
            int high, low;
            if (src + 2 >= eqpos) return 0;
            high = hex_value((unsigned char)src[1]);
            low = hex_value((unsigned char)src[2]);
            if (high < 0 || low < 0) return 0;
            chr = (unsigned char)(high * 16 + low);
            if (chr == 0) return 0;
            src += 2;
        }
        ent->name[nlen++] = (char)chr;
    }
    ent->name[nlen] = '\0';

    if (eqpos + 1 >= end || end - (eqpos + 1) > 3) return 0;
    for (src = eqpos + 1; src < end; src++) {
        if (*src < '0' || *src > '9') return 0;
        port = port * 10 + (*src - '0');
    }
    if (port >= nports) return 0;
    ent->port = (int)port;
    return 1;
}

int apple2_pad_table_decode(apple2_pad_table *tab, const char *text, int nports)
{
    const char *beg = text;
    apple2_pad_entry ent;

    apple2_pad_table_clear(tab);
    if (!text) return 0;
    while (*beg) {
        const char *end = beg;
        while (*end && *end != ';') end++;
        if (end > beg && parse_entry(&ent, beg, end, nports))
            apple2_pad_table_set(tab, ent.guid, ent.name, ent.port);
        beg = *end ? end + 1 : end;
    }
    return tab->count;
}
