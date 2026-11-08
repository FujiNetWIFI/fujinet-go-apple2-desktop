/*
 * apple2help -- the app's Help: data/help/help.md, built in by
 * tools/help/make-help.py as topics of blocks of styled spans, which every
 * frontend lays out with its own toolkit.
 *
 * A frontend shows the blocks whose platforms include its own bit. Ids are
 * GitHub's heading slugs ("controllers-and-keys"); a Help button names a
 * context instead ("prefs/Machine", "debugger/cpu"), which stays put when a
 * heading is renamed. Everything here is constant data and reentrant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPLE2HELP_H
#define APPLE2HELP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    APPLE2HELP_GNOME = 1, APPLE2HELP_KDE = 2, APPLE2HELP_WINDOWS = 4, APPLE2HELP_MACOS = 8
};

typedef enum {
    APPLE2HELP_H2 = 0, APPLE2HELP_H3,
    APPLE2HELP_PARA,
    APPLE2HELP_BULLET, APPLE2HELP_BULLET2,    /* "- " and "  - " */
    APPLE2HELP_NUMBERED,                     /* a step; number is its own */
    APPLE2HELP_NOTE,
    APPLE2HELP_PRE,                          /* one TEXT span, lines as they are */
    APPLE2HELP_TABLE_HEAD,                   /* starts a table; its rows follow */
    APPLE2HELP_TABLE_ROW
} apple2help_block_kind;

typedef enum {
    APPLE2HELP_TEXT = 0, APPLE2HELP_BOLD, APPLE2HELP_ITALIC, APPLE2HELP_CODE,
    APPLE2HELP_KBD,                          /* keys; mac_text is the Mac's */
    APPLE2HELP_LINK,                         /* target "#slug" or a URL */
    APPLE2HELP_CELL                          /* tables: the next column starts */
} apple2help_span_kind;

typedef struct {
    int kind;
    const char *text;                       /* UTF-8 */
    const char *mac_text;                   /* KBD on macOS, or NULL: text */
    const char *target;
} apple2help_span;

typedef struct {
    int kind;
    unsigned platforms;                     /* APPLE2HELP_* bits */
    int number;
    const char *id;                         /* headings: the slug */
    const char *context;                    /* headings: or NULL */
    const apple2help_span *spans;            /* headings: one TEXT span */
    int nspans;
    /* Tables: the width of each column but the last, in ems, for frontends
     * that set tab stops (the last column wraps under itself). */
    const unsigned char *col_em;
    int ncol_em;
} apple2help_block;

typedef struct {
    const char *id;
    const char *title;
    const char *context;                    /* or NULL */
    const apple2help_block *blocks;
    int nblocks;
} apple2help_topic;

int apple2help_topic_count(void);
const apple2help_topic *apple2help_topic_at(int t);

/* "slug", "#slug" or a context: its topic, and the heading's block (-1 for
 * the topic itself). 0, or -1 if there is none. */
int apple2help_find(const char *id, int *topic, int *block);

/* The span's text as platform shows it. */
const char *apple2help_span_text(const apple2help_span *sp, unsigned platform);

/* A heading block's text. */
const char *apple2help_heading(const apple2help_block *b);

/* The block after the section headed by block (the next heading at its
 * level or above, or the topic's end); -1: the whole topic. */
int apple2help_section_end(int topic, int block);

/* The section headed by block (-1: from the topic's title) as text on
 * platform: a line per block, cells separated by " | ". Returns its length
 * (truncated to dstsz - 1). */
int apple2help_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz);

/* Sections that have every word of query (case-insensitive), best first:
 * where it is in a title, then how often. block -1 is a topic's start, up
 * to its first heading. Returns how many were written. */
typedef struct {
    int topic, block, score;
} apple2help_hit;
int apple2help_search(unsigned platform, const char *query, apple2help_hit *out, int max);

/* The first match of a word of query in text at byte from or after: where
 * it starts and its length, for highlighting. 1, or 0 if there is none. */
int apple2help_match(const char *text, const char *query, int from, int *start, int *len);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2HELP_H */
