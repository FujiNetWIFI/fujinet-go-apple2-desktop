/*
 * Memory banks (apple2debug_read_bank & co.) on a running machine: main vs aux,
 * the language-card banks, ROM, absent banks, search, and the console prefixes.
 * argv[1] is the machine ("Enhanced Apple //e" by default; "Apple ][+" checks
 * that absent banks report as absent).
 *
 * Exits 77 (ctest skip) when the machine cannot boot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apple2debug.h"
#include "apple2memtools.h"
#include "apple2session.h"

static int g_fail;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            g_fail++;                                            \
        }                                                        \
    } while (0)

static uint8_t rb(apple2debug *d, int bank, uint16_t a)
{
    uint8_t b = 0;
    apple2debug_read_bank(d, bank, a, &b, 1);
    return b;
}

static const char *cmd(apple2debug *d, const char *c)
{
    static char out[4096];
    apple2debug_command(d, c, out, sizeof out);
    return out;
}

int main(int argc, char **argv)
{
    const char *machine = argc > 1 ? argv[1] : "Enhanced Apple //e";
    const int plus = strstr(machine, "][") != NULL;
    apple2session *s;
    apple2session_start_opts o;
    apple2debug *d;
    uint8_t buf[16], rom[16], cpu[16];
    uint16_t hit[8];
    int i, n, b;

    s = apple2session_new(NULL);
    if (!s) return 77;
    apple2session_default_opts(s, &o);
    o.machine = machine;
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.slot7 = "Empty";
    if (apple2session_start(s, &o) != 0) {
        fprintf(stderr, "bank_test: %s\n", apple2session_last_error(s));
        apple2session_free(s);
        return 77;
    }
    d = apple2session_debugger(s);
    if (!d) { apple2session_free(s); return 1; }
    usleep(500000);
    apple2debug_attach(d);
    for (i = 0; i < 1000 && !apple2debug_is_stopped(d); i++) usleep(2000);
    CHECK(apple2debug_is_stopped(d), "did not stop");

    /* names, prefixes, parsing */
    for (b = 0; b < APPLE2_BANK_COUNT; b++)
        CHECK(apple2debug_bank_name(b) != NULL, "name %d", b);
    CHECK(apple2debug_bank_name(APPLE2_BANK_COUNT) == NULL, "name past the end");
    CHECK(!strcmp(apple2debug_bank_name(0), "As the CPU sees it"), "name 0");
    {
        int len = 0;
        CHECK(apple2debug_bank_parse("AUX:$400", &len) == APPLE2_BANK_AUX && len == 4, "parse aux");
        CHECK(apple2debug_bank_parse("lchi:", &len) == APPLE2_BANK_LCHI && len == 5, "parse lchi");
        CHECK(apple2debug_bank_parse("label", &len) < 0, "label is no prefix");
        CHECK(apple2debug_bank_parse("$400", &len) < 0, "number is no prefix");
    }
    CHECK(apple2debug_bank_valid(d, APPLE2_BANK_CURRENT) &&
          apple2debug_bank_valid(d, APPLE2_BANK_MAIN) &&
          apple2debug_bank_valid(d, APPLE2_BANK_ROM), "current/main/rom present");
    fprintf(stderr, "bank_test: %s: aux %d lc %d auxlc %d\n", machine,
            apple2debug_bank_valid(d, APPLE2_BANK_AUX),
            apple2debug_bank_valid(d, APPLE2_BANK_LC1),
            apple2debug_bank_valid(d, APPLE2_BANK_AUXLC1));

    if (plus) {
        CHECK(!apple2debug_bank_valid(d, APPLE2_BANK_AUX), "][+ has no aux RAM");
        CHECK(!apple2debug_bank_valid(d, APPLE2_BANK_AUXLC1), "][+ has no aux LC");
        CHECK(apple2debug_read_bank(d, APPLE2_BANK_AUX, 0x400, buf, 4) == 0, "aux read on a ][+");
        CHECK(strstr(cmd(d, "mem aux:$0400 4"), "not present"), "console says absent");
        CHECK(strstr(cmd(d, "bank"), "absent"), "bank lists absent");
        apple2debug_write_bank(d, APPLE2_BANK_AUX, 0x400, 1);   /* must not crash */
        if (apple2debug_bank_valid(d, APPLE2_BANK_LC1)) {      /* AppleWin's default ][+ has a slot-0 LC */
            apple2debug_write_bank(d, APPLE2_BANK_LC1, 0xD200, 0x61);
            apple2debug_write_bank(d, APPLE2_BANK_LC2, 0xD200, 0x62);
            apple2debug_write_bank(d, APPLE2_BANK_LCHI, 0xE200, 0x63);
            CHECK(rb(d, APPLE2_BANK_LC1, 0xD200) == 0x61 && rb(d, APPLE2_BANK_LC2, 0xD200) == 0x62 &&
                  rb(d, APPLE2_BANK_LCHI, 0xE200) == 0x63, "][+ LC banks");
        }
    } else {
        /* main vs aux at the same address; the CPU shows main */
        CHECK(apple2debug_bank_valid(d, APPLE2_BANK_AUX), "//e has aux RAM");
        apple2debug_write_bank(d, APPLE2_BANK_MAIN, 0x6000, 0x11);
        apple2debug_write_bank(d, APPLE2_BANK_AUX, 0x6000, 0x22);
        CHECK(rb(d, APPLE2_BANK_MAIN, 0x6000) == 0x11, "main reads %02X", rb(d, APPLE2_BANK_MAIN, 0x6000));
        CHECK(rb(d, APPLE2_BANK_AUX, 0x6000) == 0x22, "aux reads %02X", rb(d, APPLE2_BANK_AUX, 0x6000));
        apple2debug_read(d, 0x6000, cpu, 1);
        CHECK(cpu[0] == 0x11, "CPU view %02X (main)", cpu[0]);
        /* an edit through the CPU view lands in main, not aux */
        apple2debug_write(d, 0x6001, 0x33);
        CHECK(rb(d, APPLE2_BANK_MAIN, 0x6001) == 0x33, "CPU-view write seen in main: %02X", rb(d, APPLE2_BANK_MAIN, 0x6001));
        CHECK(rb(d, APPLE2_BANK_AUX, 0x6001) != 0x33, "CPU-view write leaked into aux");
        /* zero page and stack pages hold the live copy too */
        apple2debug_write_bank(d, APPLE2_BANK_AUX, 0x0010, 0x44);
        apple2debug_write_bank(d, APPLE2_BANK_MAIN, 0x0010, 0x55);
        apple2debug_read(d, 0x0010, cpu, 1);
        CHECK(rb(d, APPLE2_BANK_AUX, 0x10) == 0x44 && rb(d, APPLE2_BANK_MAIN, 0x10) == 0x55 && cpu[0] == 0x55,
              "zp: aux %02X main %02X cpu %02X", rb(d, APPLE2_BANK_AUX, 0x10), rb(d, APPLE2_BANK_MAIN, 0x10), cpu[0]);

        /* language card banks */
        CHECK(apple2debug_bank_valid(d, APPLE2_BANK_LC1) && apple2debug_bank_valid(d, APPLE2_BANK_LC2) &&
              apple2debug_bank_valid(d, APPLE2_BANK_LCHI), "//e has a language card");
        apple2debug_write_bank(d, APPLE2_BANK_LC1, 0xD123, 0xA1);
        apple2debug_write_bank(d, APPLE2_BANK_LC2, 0xD123, 0xB2);
        apple2debug_write_bank(d, APPLE2_BANK_LCHI, 0xE123, 0xC3);
        apple2debug_write_bank(d, APPLE2_BANK_AUXLC1, 0xD123, 0xA4);
        apple2debug_write_bank(d, APPLE2_BANK_AUXLC2, 0xD123, 0xB5);
        apple2debug_write_bank(d, APPLE2_BANK_AUXLCHI, 0xE123, 0xC6);
        CHECK(rb(d, APPLE2_BANK_LC1, 0xD123) == 0xA1 && rb(d, APPLE2_BANK_LC2, 0xD123) == 0xB2 &&
              rb(d, APPLE2_BANK_LCHI, 0xE123) == 0xC3, "LC1/LC2/LCHI: %02X %02X %02X",
              rb(d, APPLE2_BANK_LC1, 0xD123), rb(d, APPLE2_BANK_LC2, 0xD123), rb(d, APPLE2_BANK_LCHI, 0xE123));
        CHECK(rb(d, APPLE2_BANK_AUXLC1, 0xD123) == 0xA4 && rb(d, APPLE2_BANK_AUXLC2, 0xD123) == 0xB5 &&
              rb(d, APPLE2_BANK_AUXLCHI, 0xE123) == 0xC6, "aux LC banks");
        /* ROM is mapped after reset: the LC writes did not show in the CPU view */
        apple2debug_read(d, 0xD123, cpu, 1);
        apple2debug_read_bank(d, APPLE2_BANK_ROM, 0xD123, rom, 1);
        CHECK(cpu[0] == rom[0], "CPU view of $D123 is %02X, ROM %02X", cpu[0], rom[0]);
        /* uncovered bytes */
        CHECK(apple2debug_read_bank(d, APPLE2_BANK_MAIN, 0xC000, buf, 4) == 0 && buf[0] == 0xFF, "main past $BFFF");
        CHECK(apple2debug_read_bank(d, APPLE2_BANK_LC1, 0xE000, buf, 4) == 0, "lc1 at $E000");
        CHECK(apple2debug_read_bank(d, APPLE2_BANK_LC1, 0xCFFE, buf, 4) == 4 && buf[0] == 0xFF &&
              buf[1] == 0xFF, "lc1 straddling $D000");
        CHECK(apple2debug_read_bank(d, APPLE2_BANK_LCHI, 0xD000, buf, 4) == 0, "lchi at $D000");

        /* search */
        {
            static const uint8_t pat[] = { 'F', 'U', 'J', 'I' };
            for (i = 0; i < 4; i++) apple2debug_write_bank(d, APPLE2_BANK_AUX, (uint16_t)(0x5000 + i), pat[i]);
            n = apple2debug_search_bank(d, APPLE2_BANK_AUX, pat, 4, 0xFF, 0, 0, 0xFFFF, hit, 8);
            CHECK(n == 1 && hit[0] == 0x5000, "aux search: n %d hit %04X", n, n > 0 ? hit[0] : 0);
            n = apple2debug_search_bank(d, APPLE2_BANK_MAIN, pat, 4, 0xFF, 0, 0, 0xFFFF, hit, 8);
            CHECK(n == 0, "main search found %d", n);
            n = apple2debug_search_bank(d, APPLE2_BANK_LC2, (const uint8_t[]){ 0xB2 }, 1, 0xFF, 0, 0, 0xFFFF, hit, 8);
            CHECK(n >= 1 && hit[0] >= 0xD000 && hit[0] < 0xE000, "lc2 search: %d", n);
        }
        /* console */
        CHECK(strstr(cmd(d, "mem aux:$6000 2"), "22"), "mem aux:");
        CHECK(strstr(cmd(d, "mem main:$6000 2"), "11"), "mem main:");
        cmd(d, "poke aux:$6002 $77 $78");
        CHECK(rb(d, APPLE2_BANK_AUX, 0x6002) == 0x77 && rb(d, APPLE2_BANK_AUX, 0x6003) == 0x78, "poke aux:");
        CHECK(strstr(cmd(d, "print [aux:$6002]"), "0077"), "print [aux:]: %s", cmd(d, "print [aux:$6002]"));
        CHECK(strstr(cmd(d, "print {aux:$6002}"), "7877"), "print {aux:}");
        CHECK(strstr(cmd(d, "print [lc2:$D123]"), "00B2"), "print [lc2:]");
        CHECK(strstr(cmd(d, "mem lc1: $D123 1"), "A1"), "mem lc1: with a space");
        CHECK(strstr(cmd(d, "find aux: text FUJI"), "$5000"), "find aux:");
        CHECK(strstr(cmd(d, "bank"), "present"), "bank");
    }

    /* ROM: bytes match the CPU view of $FA62 when ROM is mapped */
    apple2debug_read(d, 0xFA62, cpu, 16);
    CHECK(apple2debug_read_bank(d, APPLE2_BANK_ROM, 0xFA62, rom, 16) == 16 && !memcmp(cpu, rom, 16),
          "ROM $FA62 differs from the CPU view");
    apple2debug_read(d, 0xD000, cpu, 16);
    apple2debug_read_bank(d, APPLE2_BANK_ROM, 0xD000, rom, 16);
    CHECK(!memcmp(cpu, rom, 16), "ROM $D000 differs from the CPU view");
    apple2debug_write_bank(d, APPLE2_BANK_ROM, 0xFA62, (uint8_t)(rom[0] ^ 0xFF));
    apple2debug_read_bank(d, APPLE2_BANK_ROM, 0xFA62, buf, 1);
    apple2debug_read(d, 0xFA62, cpu, 1);
    CHECK(buf[0] == cpu[0], "a write to the ROM bank changed it");
    CHECK(apple2debug_read_bank(d, APPLE2_BANK_ROM, 0x8000, buf, 4) == 0, "ROM bank below $C100");
    n = apple2debug_search_bank(d, APPLE2_BANK_ROM, rom, 4, 0xFF, 0, 0xD000, 0xFFFF, hit, 8);
    CHECK(n >= 1, "ROM search: %d", n);
    {
        apple2debug_line l[3];
        apple2debug_line c[3];
        int pcl;
        n = apple2debug_disassemble_bank(d, APPLE2_BANK_ROM, 0xFA62, l, 3, &pcl);
        CHECK(n == 3 && pcl == -1 && l[0].address == 0xFA62, "disassemble rom: n %d", n);
        apple2debug_disassemble(d, 0xFA62, c, 3, &pcl);
        CHECK(!strcmp(l[0].disasm, c[0].disasm) && !strcmp(l[1].disasm, c[1].disasm), "rom disasm '%s' vs cpu '%s'",
              l[0].disasm, c[0].disasm);
    }
    CHECK(strstr(cmd(d, "disasm rom:$FA62 2"), "FA62"), "disasm rom:");
    CHECK(strstr(cmd(d, "bank"), "rom"), "bank lists rom");

    /* parse_range_bank and watches */
    {
        uint16_t lo, hi;
        int bank;
        CHECK(apple2sym_parse_range_bank(d, "aux:$0400-$04FF", &bank, &lo, &hi) && bank == APPLE2_BANK_AUX &&
              lo == 0x400 && hi == 0x4FF, "range with bank");
        CHECK(apple2sym_parse_range_bank(d, "$0400", &bank, &lo, &hi) && bank == APPLE2_BANK_CURRENT && lo == 0x400,
              "range without bank");
        CHECK(apple2sym_parse_range(d, "$0400-$0410", &lo, &hi) && lo == 0x400 && hi == 0x410, "old parse_range");
        if (!plus) {
            char line[200];
            apple2watch_clear();
            apple2watch_add("aux:$6000");
            apple2watch_format(d, 0, line, sizeof line);
            CHECK(strstr(line, "aux:$6000") && strstr(line, "$22"), "watch: '%s'", line);
            apple2watch_clear();
        }
    }

    apple2session_free(s);
    printf(g_fail ? "bank_test (%s): %d failed\n" : "bank_test (%s): ok\n", machine, g_fail ? g_fail : 0);
    return g_fail != 0;
}
