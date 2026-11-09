/*
 * Symbol tables (internal to the apple2debug engine).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2DEBUG_SYMBOLS_H
#define APPLE2DEBUG_SYMBOLS_H

#include <stdint.h>

#define SYM_MAX_TABLES 8

typedef struct {
    uint16_t addr;
    char name[48];
} sym_entry;

typedef struct {
    char name[16];
    sym_entry *entries; /* sorted by address */
    int count, cap;
    uint16_t lo, hi;
} sym_table;

typedef struct {
    sym_table tables[SYM_MAX_TABLES];
    int count;
} sym_tables;

int  symtabs_load_text(sym_tables *st, const char *text, const char *name);
int  symtabs_load_file(sym_tables *st, const char *path, const char *name);
const char *symtabs_at(const sym_tables *st, uint16_t addr, uint16_t *offset);
int  symtabs_find(const sym_tables *st, const char *name, uint16_t *addr);
void symtabs_free(sym_tables *st);
/* The name at exactly addr (user and loaded tables win over built-ins). */
const char *symtabs_exact(const sym_tables *st, uint16_t addr);
/* Set / (empty name) remove the label at addr in table "user". */
int  symtabs_set_user(sym_tables *st, uint16_t addr, const char *name);
/* The index-th name over all tables, NULL past the end (completion). */
const char *symtabs_name_at(const sym_tables *st, int index);

/* The built-in table (symbols_builtin.c). */
extern const char apple2debug_builtin_sym[];

#endif /* APPLE2DEBUG_SYMBOLS_H */
