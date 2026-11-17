/*
 * pad_assign_test -- the remembered pad -> port table, pure (no SDL, no
 * session): the text form's exact shape, the escaping of the separators and
 * control characters, decode(encode(x)) == x, lookup by GUID then by name,
 * set/remove, the cap, and that malformed input is skipped without a crash.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "pad_assign.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

#define GUID_A "030000005e040000ea02000000000000"
#define GUID_B "03000000c82d00000160000000000000"
#define GUID_C "0300000000f00000f100000000000000"

static char text[APPLE2_PAD_ASSIGN_TEXT_MAX];

int main(void)
{
    static apple2_pad_table tab, back;
    int idx, num, len;
    char guid[APPLE2_PAD_GUID_MAX];

    /* the exact text form */
    apple2_pad_table_clear(&tab);
    check(apple2_pad_table_encode(&tab, text, sizeof text) == 0 && !text[0], "an empty table is an empty string");
    check(apple2_pad_table_set(&tab, GUID_A, "Xbox Controller", 1) == 1, "set adds an entry");
    check(apple2_pad_table_set(&tab, GUID_B, "8BitDo Pro", 0) == 1, "and another");
    len = apple2_pad_table_encode(&tab, text, sizeof text);
    check(len > 0 && !strcmp(text, GUID_A "|Xbox Controller=1;" GUID_B "|8BitDo Pro=0"),
          "entries are <guid>|<name>=<port>, joined by ';', oldest first");
    check(len == (int)strlen(text), "encode returns the length");

    /* escaping */
    apple2_pad_table_clear(&tab);
    apple2_pad_table_set(&tab, GUID_A, "a;b|c=d%e\tf\n", 1);
    apple2_pad_table_encode(&tab, text, sizeof text);
    check(!strcmp(text, GUID_A "|a%3Bb%7Cc%3Dd%25e%09f%0A=1"),
          "';' '|' '=' '%' and control characters are percent-escaped");
    check(!strchr(text, '\n') && strchr(text, ';') == NULL, "so the settings line has no separator in a name");
    num = apple2_pad_table_decode(&back, text, 2);
    check(num == 1 && !strcmp(back.entry[0].name, "a;b|c=d%e\tf\n") && back.entry[0].port == 1,
          "and the name round-trips");
    apple2_pad_table_clear(&tab);
    apple2_pad_table_set(&tab, GUID_A, "Gamepad \xC3\xA9 \xE2\x80\x94", 0);
    apple2_pad_table_encode(&tab, text, sizeof text);
    check(strstr(text, "\xC3\xA9") != NULL, "UTF-8 bytes are left alone");
    apple2_pad_table_decode(&back, text, 2);
    check(back.count == 1 && !strcmp(back.entry[0].name, tab.entry[0].name), "and round-trip");
    apple2_pad_table_clear(&tab);
    apple2_pad_table_set(&tab, GUID_A, "", 1);
    apple2_pad_table_encode(&tab, text, sizeof text);
    apple2_pad_table_decode(&back, text, 2);
    check(back.count == 1 && !back.entry[0].name[0], "an empty name round-trips");

    /* round trip of a mixed table */
    apple2_pad_table_clear(&tab);
    apple2_pad_table_set(&tab, GUID_A, "One", 0);
    apple2_pad_table_set(&tab, GUID_B, "Two;2", 1);
    apple2_pad_table_set(&tab, GUID_C, "Three", 1);
    apple2_pad_table_encode(&tab, text, sizeof text);
    check(apple2_pad_table_decode(&back, text, 2) == 3 && !memcmp(&tab, &back, sizeof tab) , "decode(encode(t)) == t");

    /* lookup: GUID first, then name */
    idx = apple2_pad_table_find(&tab, GUID_B, "Whatever");
    check(idx == 1, "the GUID matches whatever the name says");
    idx = apple2_pad_table_find(&tab, GUID_C, "Three");
    check(idx == 2, "GUID and name match");
    idx = apple2_pad_table_find(&tab, "0300000099990000aaaa000000000000", "One");
    check(idx == 0, "another GUID falls back to the name");
    check(apple2_pad_table_find(&tab, "0300000099990000aaaa000000000000", "Nobody") == -1, "no GUID and no name: none");
    check(apple2_pad_table_find(&tab, NULL, "Two;2") == 1, "no GUID given: the name alone");
    strcpy(guid, "03000000C82D00000160000000000000");
    check(apple2_pad_table_find(&tab, guid, "x") == 1, "GUIDs compare without case");
    check(apple2_pad_table_find(&tab, "not hex", "Nobody") == -1, "a GUID that is not hex matches nothing");

    /* the GUID outranks the name; among equals the newest wins */
    apple2_pad_table_clear(&tab);
    apple2_pad_table_set(&tab, GUID_A, "Same", 0);
    apple2_pad_table_set(&tab, GUID_B, "Same", 1);
    check(apple2_pad_table_find(&tab, GUID_A, "Same") == 0, "the same name under two GUIDs: the GUID decides");
    check(apple2_pad_table_find(&tab, GUID_C, "Same") == 1, "a third GUID takes the newest name match");

    /* set: update in place, age, no-op */
    apple2_pad_table_clear(&tab);
    apple2_pad_table_set(&tab, GUID_A, "A", 0);
    apple2_pad_table_set(&tab, GUID_B, "B", 0);
    check(apple2_pad_table_set(&tab, GUID_A, "A", 0) == 0 && !strcmp(tab.entry[0].name, "A"),
          "the same port again changes nothing and keeps its age");
    check(apple2_pad_table_set(&tab, GUID_A, "A", 1) == 1 && tab.count == 2 && !strcmp(tab.entry[1].name, "A")
          && tab.entry[1].port == 1, "a new port updates the entry and makes it the newest");
    check(apple2_pad_table_set(&tab, "zz", "A", 1) == -1 && apple2_pad_table_set(&tab, GUID_A, "A", -1) == -1
          && apple2_pad_table_set(NULL, GUID_A, "A", 1) == -1 && apple2_pad_table_set(&tab, "", "A", 1) == -1,
          "a bad GUID, port or table is refused");
    {
        char longname[APPLE2_PAD_NAME_MAX + 8];
        memset(longname, 'n', sizeof longname - 1);
        longname[sizeof longname - 1] = '\0';
        check(apple2_pad_table_set(&tab, GUID_A, longname, 1) == -1, "a name too long is refused");
    }

    /* remove */
    check(apple2_pad_table_remove(&tab, GUID_B, "B", 0, NULL, NULL) == 1 && tab.count == 1 && !strcmp(tab.entry[0].name, "A"),
          "remove drops the exact entry");
    check(apple2_pad_table_remove(&tab, GUID_B, "B", 0, NULL, NULL) == 0, "and removing it again is a no-op");
    check(apple2_pad_table_remove(&tab, GUID_C, "A", 0, NULL, NULL) == 1 && tab.count == 0,
          "a pad known only by name: remove drops the entry find() would have matched");
    check(apple2_pad_table_remove(&tab, GUID_C, "A", 0, NULL, NULL) == 0, "from an empty table, nothing");
    apple2_pad_table_set(&tab, GUID_A, "Moved", 0);   /* the pad under a stale GUID */
    apple2_pad_table_set(&tab, GUID_B, "Moved", 1);   /* and under its current one */
    apple2_pad_table_set(&tab, GUID_C, "Other", 1);
    check(apple2_pad_table_remove(&tab, GUID_B, "Moved", 0, NULL, NULL) == 1 && tab.count == 1 && !strcmp(tab.entry[0].name, "Other")
          && apple2_pad_table_find(&tab, GUID_B, "Moved") == -1,
          "remove leaves nothing that would still apply to the pad, stale GUID included");

    /* two same-name pads under different GUIDs must not erase each other */
    {
        const char *kguid[1] = { GUID_A }, *kname[1] = { "Controller" };
        apple2_pad_table_clear(&tab);
        apple2_pad_table_set(&tab, GUID_A, "Controller", 0);
        apple2_pad_table_set(&tab, GUID_B, "Controller", 1);
        check(apple2_pad_table_remove(&tab, GUID_B, "Controller", 1, kguid, kname) == 1 && tab.count == 1
              && tab.entry[0].port == 0 && !strcmp(tab.entry[0].guid, GUID_A),
              "same name, other GUID: B removed, A's entry stays");
        check(apple2_pad_table_remove(&tab, GUID_B, "Controller", 1, kguid, kname) == 0 && tab.count == 1,
              "B automatic again: still A's entry, even via the name fallback");
        check(apple2_pad_table_remove(&tab, GUID_B, "Controller", 0, NULL, NULL) == 1 && tab.count == 0,
              "with A no longer explicit, the name-only entry goes");
    }

    /* the cap: the oldest goes */
    apple2_pad_table_clear(&tab);
    for (idx = 0; idx < APPLE2_PAD_ASSIGN_MAX + 5; idx++) {
        char name[32];
        snprintf(name, sizeof name, "Pad %d", idx);
        apple2_pad_table_set(&tab, GUID_A, name, idx & 1);
    }
    check(tab.count == APPLE2_PAD_ASSIGN_MAX, "the table holds at most APPLE2_PAD_ASSIGN_MAX entries");
    check(!strcmp(tab.entry[0].name, "Pad 5") && !strcmp(tab.entry[APPLE2_PAD_ASSIGN_MAX - 1].name, "Pad 36"),
          "the oldest five were dropped");
    check(apple2_pad_table_encode(&tab, text, sizeof text) > 0 && strlen(text) < APPLE2_PAD_ASSIGN_TEXT_MAX,
          "and the text fits its buffer");
    check(apple2_pad_table_decode(&back, text, 2) == APPLE2_PAD_ASSIGN_MAX && !memcmp(&tab, &back, sizeof tab),
          "and round-trips");
    {
        /* a file with more than the cap: the newest are kept */
        static char big[APPLE2_PAD_ASSIGN_TEXT_MAX * 2];
        size_t used = 0;
        for (idx = 0; idx < 50; idx++)
            used += (size_t)snprintf(big + used, sizeof big - used, "%s%s|Big %d=%d", idx ? ";" : "", GUID_B, idx, idx & 1);
        check(apple2_pad_table_decode(&back, big, 2) == APPLE2_PAD_ASSIGN_MAX && !strcmp(back.entry[0].name, "Big 18")
              && !strcmp(back.entry[APPLE2_PAD_ASSIGN_MAX - 1].name, "Big 49"), "decoding 50 entries keeps the newest 32");
    }
    /* the worst case still fits: every name byte escaped */
    apple2_pad_table_clear(&tab);
    for (idx = 0; idx < APPLE2_PAD_ASSIGN_MAX; idx++) {
        char name[APPLE2_PAD_NAME_MAX];
        memset(name, '%', sizeof name - 1);
        name[sizeof name - 1] = '\0';
        name[0] = (char)('A' + idx);
        apple2_pad_table_set(&tab, GUID_A, name, 1);
    }
    check(apple2_pad_table_encode(&tab, text, sizeof text) > 0, "32 names of 63 escaped bytes fit APPLE2_PAD_ASSIGN_TEXT_MAX");
    {
        char small[16];
        check(apple2_pad_table_encode(&tab, small, sizeof small) == -1 && !small[0], "a buffer too small is an error, not an overrun");
    }

    /* duplicate entries in the text: the later wins */
    num = apple2_pad_table_decode(&back, GUID_A "|Dup=0;" GUID_A "|Dup=1", 2);
    check(num == 1 && back.entry[0].port == 1, "the same device twice: the later entry wins");

    /* ports out of range are dropped */
    num = apple2_pad_table_decode(&back, GUID_A "|Low=0;" GUID_B "|High=2;" GUID_C "|Edge=1", 2);
    check(num == 2 && !strcmp(back.entry[0].name, "Low") && !strcmp(back.entry[1].name, "Edge"),
          "a port out of range is ignored and the entry dropped");
    check(apple2_pad_table_decode(&back, GUID_A "|Only=0", 0) == 0, "with no ports, every entry goes");

    /* malformed input: skipped, never fatal */
    {
        static const char *const bad[] = {
            "", ";", ";;;", "garbage", "|", "=", "|=", "|x=0", GUID_A, GUID_A "|", GUID_A "|name",
            GUID_A "|name=", GUID_A "|name=x", GUID_A "|name=-1", GUID_A "|name=1x", GUID_A "|name=0001",
            GUID_A "|name=99999999999999999999", GUID_A "|bad%=0", GUID_A "|bad%4=0", GUID_A "|bad%zz=0",
            GUID_A "|bad%00=0", "xyz|name=0", GUID_A "0|name=0",
            "0300000000000000000000000000000000000000|name=0", "%|%|%=%", "=|=|=;=|=|=", "\x01\x02\xff\xfe",
            GUID_A "|\xff\xfe=0=0", GUID_A "|a|b=0"
        };
        size_t idxb;
        int kept = 0;
        for (idxb = 0; idxb < sizeof bad / sizeof bad[0]; idxb++) {
            kept += apple2_pad_table_decode(&back, bad[idxb], 2);
            if (back.count < 0 || back.count > APPLE2_PAD_ASSIGN_MAX) kept += 1000;
        }
        check(kept == 0, "malformed entries are all skipped");
    }
    num = apple2_pad_table_decode(&back, "junk;;" GUID_A "|Good=1;|;" GUID_B "|bad%zz=0;" GUID_C "|Also good=0;", 2);
    check(num == 2 && !strcmp(back.entry[0].name, "Good") && !strcmp(back.entry[1].name, "Also good"),
          "the good entries around bad ones are kept");
    {
        char longname[200], line[400];
        memset(longname, 'q', sizeof longname - 1);
        longname[sizeof longname - 1] = '\0';
        snprintf(line, sizeof line, "%s|%s=0", GUID_A, longname);
        check(apple2_pad_table_decode(&back, line, 2) == 0, "a decoded name over the limit is dropped, not truncated");
    }
    check(apple2_pad_table_decode(&back, NULL, 2) == 0 && back.count == 0, "NULL is an empty table");
    {
        /* every byte value, and a long run, must not crash or overrun */
        char junk[2048];
        int pass;
        unsigned seed = 12345;
        for (pass = 0; pass < 200; pass++) {
            int pos;
            for (pos = 0; pos < (int)sizeof junk - 1; pos++) {
                seed = seed * 1103515245u + 12345u;
                junk[pos] = "|=;%0123456789abcdefAB \x01\xff"[(seed >> 16) % 25];
            }
            junk[sizeof junk - 1] = '\0';
            apple2_pad_table_decode(&back, junk, 2);
        }
        check(back.count >= 0 && back.count <= APPLE2_PAD_ASSIGN_MAX, "random junk leaves a sane table");
    }

    /* four players: ports 0-3 are kept, 4 is not */
    {
        char line[256];
        snprintf(line, sizeof line, "%s|P0=0;%s|P3=3;%s|P4=4", GUID_A, GUID_B, GUID_C);
        check(apple2_pad_table_decode(&back, line, 4) == 2 && back.entry[1].port == 3,
              "four players: port 3 is kept and port 4 is dropped");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
