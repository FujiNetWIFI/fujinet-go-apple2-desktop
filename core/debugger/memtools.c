/*
 * memtools -- see core/include/apple2memtools.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "apple2memtools.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- range parsing ------------------------------------------------------------ */

static int hexdigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int eval_addr(apple2debug *d, const char *text, size_t len, long *out)
{
    char buf[128], err[64];

    while (len && isspace((unsigned char)text[0])) { text++; len--; }
    while (len && isspace((unsigned char)text[len - 1])) len--;
    if (!len || len >= sizeof buf) return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';
    /* A bare 1-4 digit token is hex here (memory fields have always taken
     * "C000"), unless it names a label. */
    if (len <= 4) {
        size_t i;
        for (i = 0; i < len && hexdigit(buf[i]); i++) ;
        if (i == len && apple2debug_label_address(d, buf) < 0) {
            *out = strtol(buf, NULL, 16);
            return 1;
        }
    }
    if (apple2debug_eval(d, buf, out, err, sizeof err) != 0)
        return 0;
    *out &= 0xFFFF;
    return 1;
}

/* No operator characters: a number or a label. */
static int plain(const char *t, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (strchr("+-*/%&|^<>=!~()[]{}", t[i])) return 0;
    return 1;
}

int apple2sym_parse_range(apple2debug *d, const char *text, uint16_t *lo,
                         uint16_t *hi)
{
    const char *dots, *p;
    size_t len;
    long a, b;

    if (!d || !text) return 0;
    while (isspace((unsigned char)*text)) text++;
    len = strlen(text);
    if (!len) return 0;

    dots = strstr(text, "..");
    if (dots) {
        if (!eval_addr(d, text, (size_t)(dots - text), &a) ||
            !eval_addr(d, dots + 2, strlen(dots + 2), &b))
            return 0;
    } else {
        int split = 0;
        for (p = text + 1; *p; p++) {
            if (*p != '-') continue;
            if (plain(text, (size_t)(p - text)) && plain(p + 1, strlen(p + 1)) &&
                eval_addr(d, text, (size_t)(p - text), &a) &&
                eval_addr(d, p + 1, strlen(p + 1), &b)) {
                split = 1;
                break;
            }
        }
        if (!split) {
            if (!eval_addr(d, text, len, &a)) return 0;
            b = a;
        }
    }
    if (a > b) { long x = a; a = b; b = x; }
    *lo = (uint16_t)a;
    *hi = (uint16_t)b;
    return 1;
}

int apple2sym_parse_range_bank(apple2debug *d, const char *text, int *bank,
                               uint16_t *lo, uint16_t *hi)
{
    int len = 0, b;

    if (bank) *bank = APPLE2_BANK_CURRENT;
    if (!d || !text) return 0;
    while (isspace((unsigned char)*text)) text++;
    b = apple2debug_bank_parse(text, &len);
    if (b > 0) {
        text += len;
        if (bank) *bank = b;
    }
    return apple2sym_parse_range(d, text, lo, hi);
}

/* ---- watches ------------------------------------------------------------------ */

static struct {
    pthread_mutex_t mtx;
    char text[APPLE2WATCH_MAX][96];
    int count;
    unsigned generation;
} g_watch = { .mtx = PTHREAD_MUTEX_INITIALIZER };

int apple2watch_count(void)
{
    int n;
    pthread_mutex_lock(&g_watch.mtx);
    n = g_watch.count;
    pthread_mutex_unlock(&g_watch.mtx);
    return n;
}

int apple2watch_add(const char *text)
{
    int idx = -1;

    if (!text) return -1;
    while (isspace((unsigned char)*text)) text++;
    if (!*text) return -1;
    pthread_mutex_lock(&g_watch.mtx);
    if (g_watch.count < APPLE2WATCH_MAX) {
        size_t n;
        idx = g_watch.count++;
        snprintf(g_watch.text[idx], sizeof g_watch.text[idx], "%s", text);
        n = strlen(g_watch.text[idx]);
        while (n && isspace((unsigned char)g_watch.text[idx][n - 1]))
            g_watch.text[idx][--n] = '\0';
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
    return idx;
}

void apple2watch_remove(int index)
{
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) {
        memmove(g_watch.text[index], g_watch.text[index + 1],
                (size_t)(g_watch.count - index - 1) * sizeof g_watch.text[0]);
        g_watch.count--;
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
}

void apple2watch_clear(void)
{
    pthread_mutex_lock(&g_watch.mtx);
    g_watch.count = 0;
    g_watch.generation++;
    pthread_mutex_unlock(&g_watch.mtx);
}

/* Valid until the next change; the windows are single-threaded users. */
const char *apple2watch_text(int index)
{
    const char *t = NULL;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) t = g_watch.text[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return t;
}

unsigned apple2watch_generation(void)
{
    unsigned g;
    pthread_mutex_lock(&g_watch.mtx);
    g = g_watch.generation;
    pthread_mutex_unlock(&g_watch.mtx);
    return g;
}

int apple2watch_range_bank(apple2debug *d, int index, int *bank, uint16_t *lo,
                           uint16_t *hi)
{
    char text[96];
    const char *t = apple2watch_text(index);

    if (bank) *bank = APPLE2_BANK_CURRENT;
    if (!t) return 0;
    snprintf(text, sizeof text, "%s", t);
    return apple2sym_parse_range_bank(d, text, bank, lo, hi);
}

int apple2watch_range(apple2debug *d, int index, uint16_t *lo, uint16_t *hi)
{
    return apple2watch_range_bank(d, index, NULL, lo, hi);
}

int apple2watch_format(apple2debug *d, int index, char *dst, int dstsz)
{
    char text[96], name[48] = "";
    uint16_t lo, hi;
    const char *t = apple2watch_text(index);
    int n = 0, i, bank = APPLE2_BANK_CURRENT;
    const char *pre;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!t) return 0;
    snprintf(text, sizeof text, "%s", t);
    if (!apple2sym_parse_range_bank(d, text, &bank, &lo, &hi))
        return snprintf(dst, (size_t)dstsz, "%-14s ?", text);
    pre = apple2debug_bank_prefix(bank);
    apple2debug_address_label(d, lo, name, sizeof name);
    n = snprintf(dst, (size_t)dstsz, "%-14s %s%s$%04X", text, pre ? pre : "",
                 pre && *pre ? ":" : "", lo);
    if (n >= dstsz) return dstsz - 1;
    if (!apple2debug_is_stopped(d))
        return n + snprintf(dst + n, (size_t)(dstsz - n), "  -");
    if (lo == hi) {
        uint8_t b = 0;
        if (!apple2debug_read_bank(d, bank, lo, &b, 1))
            return n + snprintf(dst + n, (size_t)(dstsz - n), "  -");
        return n + snprintf(dst + n, (size_t)(dstsz - n), "  $%02X (%u)", b, b);
    }
    n += snprintf(dst + n, (size_t)(dstsz - n), "-$%04X ", hi);
    for (i = 0; i < 8 && lo + i <= hi && n < dstsz - 4; i++) {
        uint8_t b = 0;
        apple2debug_read_bank(d, bank, (uint16_t)(lo + i), &b, 1);
        n += snprintf(dst + n, (size_t)(dstsz - n), " %02X", b);
    }
    if (hi - lo >= 8 && n < dstsz - 4)
        n += snprintf(dst + n, (size_t)(dstsz - n), " ...");
    return n < dstsz ? n : dstsz - 1;
}

/* ---- search ------------------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int apple2search_parse(int kind, const char *text, uint8_t *pat, int *n,
                      uint8_t *mask, int *flags)
{
    int cnt = 0;

    if (!text || !pat || !n || !mask || !flags) return 0;
    *mask = 0xFF;
    *flags = 0;
    if (kind == APPLE2SEARCH_TEXT) {
        const char *p;
        for (p = text; *p && cnt < APPLE2SEARCH_MAX; p++)
            pat[cnt++] = (uint8_t)*p;
        *flags = APPLE2DEBUG_SEARCH_NOCASE;
    } else {
        const char *p = text;
        while (*p) {
            int hi, lo;
            if (isspace((unsigned char)*p) || *p == ',') { p++; continue; }
            if (*p == '$') { p++; continue; }
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; continue; }
            hi = hexval(*p);
            if (hi < 0) return 0;
            lo = hexval(p[1]);
            if (lo < 0) { lo = hi; hi = 0; p += 1; }   /* a lone digit is a byte */
            else p += 2;
            if (cnt >= APPLE2SEARCH_MAX) break;
            pat[cnt++] = (uint8_t)(hi << 4 | lo);
        }
    }
    *n = cnt;
    return cnt > 0;
}

/* ---- breakpoint description ----------------------------------------------------- */

int apple2bp_describe(apple2debug *d, const apple2debug_breakpoint *b, char *dst,
                     int dstsz)
{
    static const struct { int bit; const char *name; } types[] = {
        { APPLE2DEBUG_BP_EXEC, "Exec" }, { APPLE2DEBUG_BP_READ, "Read" },
        { APPLE2DEBUG_BP_WRITE, "Write" },
    };
    char ty[32] = "", lab[48] = "";
    int n, i;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!b) return 0;
    for (i = 0; i < 3; i++)
        if (b->type & types[i].bit) {
            if (ty[0]) strncat(ty, "/", sizeof ty - strlen(ty) - 1);
            strncat(ty, types[i].name, sizeof ty - strlen(ty) - 1);
        }
    apple2debug_address_label(d, b->start, lab, sizeof lab);
    if (b->start == b->end)
        n = snprintf(dst, (size_t)dstsz, "%s  $%04X", ty,
                     b->start);
    else
        n = snprintf(dst, (size_t)dstsz, "%s  $%04X-$%04X",
                     ty, b->start, b->end);
    if (n < dstsz && lab[0])
        n += snprintf(dst + n, (size_t)(dstsz - n), " (%s)", lab);
    if (n < dstsz && b->condition[0])
        n += snprintf(dst + n, (size_t)(dstsz - n), "  if %s", b->condition);
    return n < dstsz ? n : dstsz - 1;
}
