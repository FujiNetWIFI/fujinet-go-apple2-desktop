/*
 * Symbol tables for the apple2debug engine. Parses AppleWin's ".SYM" format ("HHHH NAME" per line)
 * ("HHHH NAME [; comment]" per line, also ca65 .lbl, WLA-DX, z88dk .map and
 * SDCC .noi lines) from files or the built-in tables.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "symbols.h"

static int cmp_addr(const void *a, const void *b)
{
    const sym_entry *sa = a, *sb = b;
    return (int)sa->addr - (int)sb->addr;
}

static void table_add(sym_table *t, uint16_t addr, const char *name)
{
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 256;
        sym_entry *e = realloc(t->entries, (size_t)cap * sizeof(*e));
        if (!e) return;
        t->entries = e;
        t->cap = cap;
    }
    t->entries[t->count].addr = addr;
    snprintf(t->entries[t->count].name, sizeof(t->entries[t->count].name),
             "%.47s", name);
    t->count++;
}

/* One symbol per line, in any of the formats the toolchains write:
 *   "HHHH NAME [; comment]"    the plain list
 *   "BB:AAAA name"             WLA-DX (the bank is ignored)
 *   "name = $AAAA ; ..."       z88dk .map
 *   "DEF name 0xAAAA"          SDCC .noi
 * Blank lines, '#' and ';' comments and "[section]" headers are skipped. */
static int parse_line(const char *line, unsigned *addr, char name[64])
{
    const char *p = line;
    unsigned bank;

    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#' || *p == ';' || *p == '[' || *p == '\r')
        return 0;
    if (strncmp(p, "al ", 3) == 0) {          /* ca65 .lbl: al 00C000 .name */
        if (sscanf(p + 3, "%x %63s", addr, name) != 2) return 0;
        if (name[0] == '.') memmove(name, name + 1, strlen(name));
        return name[0] != '\0';
    }
    if (strncmp(p, "DEF ", 4) == 0)
        return sscanf(p + 4, "%63s %x", name, addr) == 2;
    if (sscanf(p, "%63s = $%x", name, addr) == 2)
        return 1;
    if (sscanf(p, "%x:%x %63s", &bank, addr, name) == 3)
        return 1;
    return sscanf(p, "%x %63s", addr, name) == 2 && name[0] != ';';
}

static void table_finish(sym_table *t)
{
    qsort(t->entries, (size_t)t->count, sizeof(sym_entry), cmp_addr);
    t->lo = t->hi = 0;
    if (t->count) {
        t->lo = t->entries[0].addr;
        t->hi = t->entries[t->count - 1].addr;
    }
}

static void table_parse(sym_table *t, const char *text)
{
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[160];
        unsigned addr;
        char name[64];

        if (len < sizeof(line)) {
            memcpy(line, p, len);
            line[len] = '\0';
            if (parse_line(line, &addr, name) && addr <= 0xFFFF)
                table_add(t, (uint16_t)addr, name);
        }
        if (!eol) break;
        p = eol + 1;
    }
    table_finish(t);
}

int symtabs_load_text(sym_tables *st, const char *text, const char *name)
{
    sym_table *t;
    int i;

    /* Reloading a table of the same name replaces it. */
    for (i = 0; i < st->count; i++)
        if (strcmp(st->tables[i].name, name) == 0)
            break;
    if (i == st->count) {
        if (st->count == SYM_MAX_TABLES) return -1;
        st->count++;
    }
    t = &st->tables[i];
    free(t->entries);
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", name);
    table_parse(t, text);
    return t->count;
}

int symtabs_load_file(sym_tables *st, const char *path, const char *name)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    char *text;
    int rc;

    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    text = malloc((size_t)sz + 1);
    if (!text) {
        fclose(fp);
        return -1;
    }
    if (fread(text, 1, (size_t)sz, fp) != (size_t)sz) {
        free(text);
        fclose(fp);
        return -1;
    }
    text[sz] = '\0';
    fclose(fp);
    rc = symtabs_load_text(st, text, name);
    free(text);
    return rc;
}

/* Exact-or-nearest-below lookup, preferring the table whose address span
 * contains addr (EOS lives high, OS7 low; they don't overlap). */
const char *symtabs_at(const sym_tables *st, uint16_t addr, uint16_t *offset)
{
    const sym_entry *best = NULL;
    int best_in_span = 0;
    int i;

    for (i = 0; i < st->count; i++) {
        const sym_table *t = &st->tables[i];
        int lo = 0, hi = t->count - 1, found = -1;
        int in_span = t->count && addr >= t->lo && addr <= t->hi;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (t->entries[mid].addr <= addr) {
                found = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (found < 0) continue;
        if (!best || (in_span && !best_in_span) ||
            (in_span == best_in_span && t->entries[found].addr > best->addr)) {
            best = &t->entries[found];
            best_in_span = in_span;
        }
    }
    if (!best) return NULL;
    if (offset) *offset = (uint16_t)(addr - best->addr);
    return best->name;
}

int symtabs_find(const sym_tables *st, const char *name, uint16_t *addr)
{
    int i, j;
    for (i = 0; i < st->count; i++)
        for (j = 0; j < st->tables[i].count; j++)
            if (strcmp(st->tables[i].entries[j].name, name) == 0) {
                if (addr) *addr = st->tables[i].entries[j].addr;
                return 1;
            }
    return 0;
}

/* The name at exactly addr; later tables (user, file) win over the
 * built-ins. */
const char *symtabs_exact(const sym_tables *st, uint16_t addr)
{
    int i;
    for (i = st->count - 1; i >= 0; i--) {
        const sym_table *t = &st->tables[i];
        int lo = 0, hi = t->count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (t->entries[mid].addr == addr)
                return t->entries[mid].name;
            if (t->entries[mid].addr < addr)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    return NULL;
}

/* Sets (or, with an empty name, removes) the user label at addr. */
int symtabs_set_user(sym_tables *st, uint16_t addr, const char *name)
{
    sym_table *t = NULL;
    int i, j;

    for (i = 0; i < st->count; i++)
        if (strcmp(st->tables[i].name, "user") == 0)
            t = &st->tables[i];
    if (!t) {
        if (!name || !*name) return 0;
        if (st->count == SYM_MAX_TABLES) return -1;
        t = &st->tables[st->count++];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "user");
    }
    for (j = 0; j < t->count; ) {
        if (t->entries[j].addr == addr) {
            memmove(&t->entries[j], &t->entries[j + 1],
                    (size_t)(t->count - j - 1) * sizeof(sym_entry));
            t->count--;
        } else {
            j++;
        }
    }
    if (name && *name)
        table_add(t, addr, name);
    table_finish(t);
    return 0;
}

/* The index-th name across all tables, for completion; NULL past the end. */
const char *symtabs_name_at(const sym_tables *st, int index)
{
    int i;
    for (i = 0; i < st->count; i++) {
        if (index < st->tables[i].count)
            return st->tables[i].entries[index].name;
        index -= st->tables[i].count;
    }
    return NULL;
}

void symtabs_free(sym_tables *st)
{
    int i;
    for (i = 0; i < st->count; i++)
        free(st->tables[i].entries);
    memset(st, 0, sizeof(*st));
}
