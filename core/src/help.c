/*
 * The Help's topics (help_data.h, generated from data/help/help.md):
 * lookup by id or context, plain text, and search.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "help_data.h"

#define MAX_WORDS 8
#define WORD_MAX 64

int
apple2help_topic_count(void)
{
    return apple2help_topics_count;
}

const apple2help_topic *
apple2help_topic_at(int t)
{
    return (t >= 0 && t < apple2help_topics_count) ? &apple2help_topics_data[t] : NULL;
}

static int
same(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

int
apple2help_find(const char *id, int *topic, int *block)
{
    if (!id || !*id)
        return -1;
    if (*id == '#')
        id++;
    for (int t = 0; t < apple2help_topics_count; t++) {
        const apple2help_topic *tp = &apple2help_topics_data[t];
        int b = -2;
        if (same(tp->id, id) || same(tp->context, id))
            b = -1;
        for (int k = 0; b == -2 && k < tp->nblocks; k++)
            if (same(tp->blocks[k].id, id) || same(tp->blocks[k].context, id))
                b = k;
        if (b != -2) {
            if (topic)
                *topic = t;
            if (block)
                *block = b;
            return 0;
        }
    }
    return -1;
}

const char *
apple2help_span_text(const apple2help_span *sp, unsigned platform)
{
    return ((platform & APPLE2HELP_MACOS) && sp->mac_text) ? sp->mac_text : sp->text;
}

const char *
apple2help_heading(const apple2help_block *b)
{
    return (b && b->nspans) ? b->spans[0].text : "";
}

static int
level(int kind)
{
    return kind == APPLE2HELP_H2 ? 2 : kind == APPLE2HELP_H3 ? 3 : 0;
}

int
apple2help_section_end(int topic, int block)
{
    const apple2help_topic *tp = apple2help_topic_at(topic);
    if (!tp)
        return 0;
    if (block < 0)
        return tp->nblocks;
    int lv = level(tp->blocks[block].kind);
    if (!lv)
        return block + 1;
    for (int b = block + 1; b < tp->nblocks; b++) {
        int l = level(tp->blocks[b].kind);
        if (l && l <= lv)
            return b;
    }
    return tp->nblocks;
}

/* ---- plain text ----------------------------------------------------------- */

typedef struct {
    char *dst;
    int size, len;
} sink;

static void
put(sink *o, const char *s)
{
    size_t n = strlen(s);
    if (o->len + 1 >= o->size)
        return;
    if (n > (size_t) (o->size - 1 - o->len))
        n = (size_t) (o->size - 1 - o->len);
    memcpy(o->dst + o->len, s, n);
    o->len += (int) n;
    o->dst[o->len] = 0;
}

static void
put_block(sink *o, const apple2help_block *b, unsigned platform)
{
    for (int i = 0; i < b->nspans; i++)
        put(o, b->spans[i].kind == APPLE2HELP_CELL ? " | " : apple2help_span_text(&b->spans[i], platform));
    put(o, "\n");
}

int
apple2help_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz)
{
    const apple2help_topic *tp = apple2help_topic_at(topic);
    sink o = { dst, dstsz, 0 };
    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = 0;
    if (!tp)
        return 0;
    if (block < 0) {
        put(&o, tp->title);
        put(&o, "\n");
    }
    int end = apple2help_section_end(topic, block);
    for (int b = block < 0 ? 0 : block; b < end; b++)
        if (tp->blocks[b].platforms & platform)
            put_block(&o, &tp->blocks[b], platform);
    return o.len;
}

/* ---- search ------------------------------------------------------------------ */

static int
lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Where word (already lower case) is in s from from on, or -1. */
static int
find_ci(const char *s, const char *word, int from)
{
    size_t n = strlen(word);
    for (int i = from; s[i]; i++) {
        size_t k = 0;
        while (k < n && s[i + k] && lower((unsigned char) s[i + k]) == (unsigned char) word[k])
            k++;
        if (k == n)
            return i;
    }
    return -1;
}

static int
count_ci(const char *s, const char *word, int cap)
{
    int n = 0, at = 0;
    while (n < cap && (at = find_ci(s, word, at)) >= 0) {
        n++;
        at++;
    }
    return n;
}

static int
split(const char *query, char words[MAX_WORDS][WORD_MAX])
{
    int n = 0;
    const char *p = query ? query : "";
    while (*p && n < MAX_WORDS) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        int k = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
            if (k < WORD_MAX - 1)
                words[n][k++] = (char) lower((unsigned char) *p);
            p++;
        }
        if (k) {
            words[n][k] = 0;
            n++;
        }
    }
    return n;
}

/* A search unit: a heading up to the next heading of any level. */
static int
unit_end(const apple2help_topic *tp, int u)
{
    for (int b = u + 1; b < tp->nblocks; b++)
        if (level(tp->blocks[b].kind))
            return b;
    return tp->nblocks;
}

static int
body_count(const apple2help_topic *tp, int first, int end, unsigned platform, const char *word)
{
    int n = 0;
    for (int b = first; b < end && n < 5; b++) {
        const apple2help_block *bl = &tp->blocks[b];
        if (!(bl->platforms & platform))
            continue;
        for (int i = 0; i < bl->nspans && n < 5; i++)
            n += count_ci(apple2help_span_text(&bl->spans[i], platform), word, 5 - n);
    }
    return n;
}

static int
unit_score(const apple2help_topic *tp, int u, int end, unsigned platform,
           char words[MAX_WORDS][WORD_MAX], int nw)
{
    const char *title = u < 0 ? tp->title : apple2help_heading(&tp->blocks[u]);
    int score = 0, own = 0;
    for (int w = 0; w < nw; w++) {
        int here = find_ci(title, words[w], 0) >= 0;
        int topic = find_ci(tp->title, words[w], 0) >= 0;
        int body = body_count(tp, u + 1, end, platform, words[w]);
        if (!here && !topic && !body)
            return 0;
        own |= here || body;
        score += here * 20 + topic * 5 + body;
    }
    return own ? score : 0;
}

int
apple2help_search(unsigned platform, const char *query, apple2help_hit *out, int max)
{
    char words[MAX_WORDS][WORD_MAX];
    int nw = split(query, words), n = 0;
    if (!nw || !out || max <= 0)
        return 0;
    for (int t = 0; t < apple2help_topics_count; t++) {
        const apple2help_topic *tp = &apple2help_topics_data[t];
        for (int u = -1; u < tp->nblocks;) {
            int end = unit_end(tp, u);
            int score = (u < 0 || (tp->blocks[u].platforms & platform))
                ? unit_score(tp, u, end, platform, words, nw) : 0;
            if (score && (n < max || score > out[n - 1].score)) {
                int at = n < max ? n++ : max - 1;
                while (at > 0 && out[at - 1].score < score) {
                    out[at] = out[at - 1];
                    at--;
                }
                out[at].topic = t;
                out[at].block = u;
                out[at].score = score;
            }
            u = end;
        }
    }
    return n;
}

int
apple2help_match(const char *text, const char *query, int from, int *start, int *len)
{
    char words[MAX_WORDS][WORD_MAX];
    int nw = split(query, words), best = -1, blen = 0;
    if (!text || from < 0 || (size_t) from > strlen(text))
        return 0;
    for (int w = 0; w < nw; w++) {
        int at = find_ci(text, words[w], from);
        if (at >= 0 && (best < 0 || at < best || (at == best && (int) strlen(words[w]) > blen))) {
            best = at;
            blen = (int) strlen(words[w]);
        }
    }
    if (best < 0)
        return 0;
    if (start)
        *start = best;
    if (len)
        *len = blen;
    return 1;
}
