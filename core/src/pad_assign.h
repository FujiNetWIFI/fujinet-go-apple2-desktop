/*
 * pad_assign -- the remembered pad -> port assignments, as a pure table with
 * a text form for the settings key "pad_assign". No SDL and no session: the
 * gamepad layer passes the SDL GUID as its 32-character hex string.
 *
 * Text form: entries separated by ';', each
 *     <guid>|<percent-encoded name>=<port>
 * The name escapes ';' '|' '=' '%' and control characters (below 0x20, 0x7F)
 * as %XX, so an entry never holds a separator and the settings file stays one
 * line. The table is ordered oldest first and holds at most
 * APPLE2_PAD_ASSIGN_MAX entries: the oldest is dropped to make room.
 *
 * A device is its GUID plus its name. find() matches the GUID first (the
 * newest entry with that GUID) and falls back to the name alone (some
 * systems give the same pad another GUID in another USB port). Two identical
 * pads (same GUID and name) cannot be told apart: they share one entry, and
 * the gamepad layer applies it to the first one that connects.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_PAD_ASSIGN_H
#define APPLE2_PAD_ASSIGN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APPLE2_PAD_ASSIGN_MAX   32
#define APPLE2_PAD_GUID_MAX     33    /* 32 hex digits and the NUL */
#define APPLE2_PAD_NAME_MAX     64    /* with the NUL, as the gamepad layer's names */
/* Room for a full table encoded with every name byte escaped. */
#define APPLE2_PAD_ASSIGN_TEXT_MAX 8192

typedef struct {
    char guid[APPLE2_PAD_GUID_MAX];   /* lower-case hex */
    char name[APPLE2_PAD_NAME_MAX];
    int  port;                     /* 0 or more */
} apple2_pad_entry;

typedef struct {
    apple2_pad_entry entry[APPLE2_PAD_ASSIGN_MAX];   /* oldest first */
    int count;
} apple2_pad_table;

void apple2_pad_table_clear(apple2_pad_table *tab);

/* Replaces the table with what `text` holds (NULL or "" = empty). Garbage is
 * skipped entry by entry, never fatal: a bad GUID, a bad escape, a name too
 * long, a port that is not a number or not below `nports`. Later entries
 * for the same GUID and name win. Returns the entries kept. */
int apple2_pad_table_decode(apple2_pad_table *tab, const char *text, int nports);

/* Writes the text form to dst (NUL-terminated); returns its length, or -1 if
 * dst (dstsz bytes) is too small. APPLE2_PAD_ASSIGN_TEXT_MAX always fits. */
int apple2_pad_table_encode(const apple2_pad_table *tab, char *dst, size_t dstsz);

/* The entry for a device: GUID first, then name. Index or -1. */
int apple2_pad_table_find(const apple2_pad_table *tab, const char *guid, const char *name);

/* Sets the port of the entry with exactly this GUID and name, or appends one
 * (the oldest goes if the table is full). An unchanged port is a no-op that
 * keeps the entry's age. Returns 1 if the table changed, 0 if not, -1 on a
 * bad argument (a GUID that is not hex, a negative port, a name too long). */
int apple2_pad_table_set(apple2_pad_table *tab, const char *guid, const char *name, int port);

/* Forgets a device: every entry find() would match for it (its own, and a
 * stale one under another GUID with the same name), so none is left to apply
 * the next run. The nkeep devices (keepguid[idx], keepname[idx]) are the other
 * pads that still hold an explicit assignment: an entry find() matches for
 * one of them stays, so two same-name pads with different GUIDs never erase
 * each other. Returns 1 if the table changed, else 0. */
int apple2_pad_table_remove(apple2_pad_table *tab, const char *guid, const char *name,
                         int nkeep, const char *const *keepguid, const char *const *keepname);

#ifdef __cplusplus
}
#endif

#endif /* APPLE2_PAD_ASSIGN_H */
