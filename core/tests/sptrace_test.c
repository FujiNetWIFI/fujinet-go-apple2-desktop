/*
 * Unit tests for the debugger's FujiNet trace decoder: SmartPort-over-SLIP
 * requests and replies fed as bytes, the way apple2_host's trace seam hands
 * them over. No emulator, ROMs or network.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sptrace.h"

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

static apple2_fuji_packet g_p[SPTRACE_MAX];

static int copy(sptrace *t)
{
    return sptrace_copy(t, g_p, SPTRACE_MAX);
}

/* A STATUS or CONTROL request: [seq, cmd, pcount, unit, 0, 0, code, 0,
 * 0, 0, 0] and, for CONTROL, the payload's length and bytes. */
static void request(sptrace *t, uint8_t seq, uint8_t cmd, uint8_t unit,
                    uint8_t code, int payload)
{
    uint8_t b[64] = { 0 };
    size_t n = 11;
    b[0] = seq;
    b[1] = cmd;
    b[2] = 3;
    b[3] = unit;
    b[6] = code;
    if (payload >= 0) {
        b[11] = (uint8_t)payload;
        b[12] = 0;
        n = 13 + (size_t)payload;
    }
    sptrace_feed(t, 0, seq, b, n);
}

/* A reply: [seq, status, data...]. */
static void reply(sptrace *t, uint8_t seq, uint8_t status, int ndata)
{
    uint8_t b[600];
    int i;
    b[0] = seq;
    b[1] = status;
    for (i = 0; i < ndata; i++) b[2 + i] = (uint8_t)i;
    sptrace_feed(t, 1, seq, b, (size_t)ndata + 2);
}

/* A DIB reply naming the unit. */
static void dib(sptrace *t, uint8_t seq, const char *name)
{
    uint8_t b[27] = { 0 };
    size_t len = strlen(name);
    b[0] = seq;
    b[1] = 0;
    b[2] = 0xF8;
    b[6] = (uint8_t)len;
    memset(b + 7, ' ', 16);
    memcpy(b + 7, name, len);
    sptrace_feed(t, 1, seq, b, sizeof b);
}

static int has(const char *text, const char *want)
{
    return strstr(text, want) != NULL;
}

int main(void)
{
    sptrace *t = sptrace_new();
    char state[2048];
    int n, i;
    uint64_t serial;

    CHECK(t != NULL, "no trace");
    if (!t) return 1;

    /* ---- empty --------------------------------------------------------- */
    CHECK(copy(t) == 0, "a new trace is not empty");
    sptrace_describe(t, state, sizeof state);
    CHECK(has(state, "Requests   0") && has(state, "(none yet)"),
          "empty state text: %s", state);

    /* ---- both directions, and a unit learning its name ----------------- */
    request(t, 0x10, 0, 1, 3, -1);          /* STATUS u1 DIB */
    dib(t, 0x10, "THE_FUJI");
    n = copy(t);
    CHECK(n == 2, "DIB exchange gave %d packets, want 2", n);
    CHECK(!g_p[0].to_host && g_p[1].to_host, "directions wrong");
    CHECK(strcmp(g_p[0].text, "\xe2\x86\x92 #10 STATUS u1 03 DIB") == 0,
          "request text \"%s\"", g_p[0].text);
    CHECK(strcmp(g_p[1].text, "\xe2\x86\x90 #10 OK 25 B THE_FUJI") == 0,
          "DIB reply text \"%s\"", g_p[1].text);
    CHECK(g_p[0].device == 1 && g_p[0].command == 0 && g_p[0].nbytes == 11,
          "request fields %d %d %d", g_p[0].device, g_p[0].command,
          g_p[0].nbytes);
    CHECK(g_p[1].device == 1 && g_p[1].command == 0 && g_p[1].length == 25,
          "reply fields %d %d %d", g_p[1].device, g_p[1].command,
          g_p[1].length);
    CHECK(g_p[0].serial < g_p[1].serial, "serials do not rise");

    /* On THE_FUJI a status code is a Fuji command... */
    request(t, 0x2A, 0, 1, 0xE8, -1);
    reply(t, 0x2A, 0, 140);
    n = copy(t);
    CHECK(strcmp(g_p[n - 2].text,
                 "\xe2\x86\x92 #2A STATUS u1 THE_FUJI E8 GET ADAPTERCONFIG") == 0,
          "Fuji status text \"%s\"", g_p[n - 2].text);
    CHECK(strcmp(g_p[n - 1].text, "\xe2\x86\x90 #2A OK 140 B") == 0,
          "reply text \"%s\"", g_p[n - 1].text);
    CHECK(g_p[n - 1].nbytes == 142 && g_p[n - 1].data[0] == 0x2A &&
          g_p[n - 1].data[2] == 0 && g_p[n - 1].data[63] == 61,
          "reply bytes not kept");

    /* ...and a CONTROL carries its payload length. */
    request(t, 0x2B, 4, 1, 0xF9, 1);
    reply(t, 0x2B, 0, 0);
    n = copy(t);
    CHECK(strcmp(g_p[n - 2].text,
                 "\xe2\x86\x92 #2B CONTROL u1 THE_FUJI F9 MOUNT HOST (1 B)") == 0,
          "Fuji control text \"%s\"", g_p[n - 2].text);
    CHECK(strcmp(g_p[n - 1].text, "\xe2\x86\x90 #2B OK") == 0,
          "empty reply text \"%s\"", g_p[n - 1].text);

    /* Elsewhere codes are SmartPort's, or a network unit's letters. */
    request(t, 0x2C, 4, 5, 'O', 0);
    reply(t, 0x2C, 0x28, 0);
    n = copy(t);
    CHECK(strcmp(g_p[n - 2].text, "\xe2\x86\x92 #2C CONTROL u5 4F 'O'") == 0,
          "network control text \"%s\"", g_p[n - 2].text);
    CHECK(strcmp(g_p[n - 1].text, "\xe2\x86\x90 #2C NODRIVE") == 0,
          "error reply text \"%s\"", g_p[n - 1].text);

    /* READ BLOCK names its 24-bit block. */
    {
        uint8_t b[11] = { 0x2D, 1, 3, 2, 0, 0, 0x12, 0x34, 0x01, 0, 0 };
        sptrace_feed(t, 0, 0x2D, b, sizeof b);
        reply(t, 0x2D, 0, 512);
        n = copy(t);
        CHECK(strcmp(g_p[n - 2].text,
                     "\xe2\x86\x92 #2D READ BLOCK u2 blk 78866") == 0,
              "read block text \"%s\"", g_p[n - 2].text);
        CHECK(g_p[n - 1].nbytes == 514, "block reply is %d bytes",
              g_p[n - 1].nbytes);
    }

    /* No reply in time. */
    request(t, 0x2E, 0, 1, 0xFA, -1);
    sptrace_feed(t, 2, 0x2E, NULL, 0);
    n = copy(t);
    CHECK(strcmp(g_p[n - 1].text, "\xe2\x86\x90 #2E TIMEOUT") == 0,
          "timeout text \"%s\"", g_p[n - 1].text);
    CHECK(g_p[n - 1].to_host && g_p[n - 1].nbytes == 0 &&
          g_p[n - 1].device == 1, "timeout fields");

    /* ---- short and stray packets --------------------------------------- */
    {
        const uint8_t shortreq[2] = { 0x30, 0 }, shortrep[1] = { 0x31 };
        sptrace_feed(t, 0, 0x30, shortreq, sizeof shortreq);
        sptrace_feed(t, 1, 0x31, shortrep, sizeof shortrep);
        reply(t, 0x77, 0, 4);   /* nothing asked for it */
        n = copy(t);
        CHECK(strcmp(g_p[n - 3].text, "\xe2\x86\x92 #30 2 B (short)") == 0,
              "short request text \"%s\"", g_p[n - 3].text);
        CHECK(strcmp(g_p[n - 2].text, "\xe2\x86\x90 #31 1 B (short)") == 0,
              "short reply text \"%s\"", g_p[n - 2].text);
        CHECK(g_p[n - 1].device == -1 && has(g_p[n - 1].text, "OK 4 B"),
              "stray reply \"%s\" device %d", g_p[n - 1].text,
              g_p[n - 1].device);
    }

    /* ---- two threads' exchanges interleaved ---------------------------- */
    {
        const int before = copy(t);
        request(t, 0x40, 0, 1, 0xE8, -1);   /* the card */
        request(t, 0x41, 5, 2, 0, -1);      /* the Listener's INIT scan */
        reply(t, 0x40, 0, 140);
        reply(t, 0x41, 0, 0);
        n = copy(t);
        CHECK(n == before + 4, "interleaved exchanges gave %d packets, want %d",
              n - before, 4);
        CHECK(has(g_p[n - 3].text, "#41 INIT u2"), "INIT text \"%s\"",
              g_p[n - 3].text);
    }

    /* ---- identical exchanges fold ---------------------------------------- */
    sptrace_clear(t);
    CHECK(copy(t) == 0, "clear left packets");
    sptrace_describe(t, state, sizeof state);
    CHECK(has(state, "Requests   0") && has(state, "  1  THE_FUJI"),
          "cleared state text: %s", state);

    for (i = 0; i < 5; i++) {
        request(t, (uint8_t)(0x50 + i), 0, 1, 0xFA, -1);
        reply(t, (uint8_t)(0x50 + i), 0, 2);
    }
    n = copy(t);
    CHECK(n == 2, "five identical exchanges gave %d packets, want 2", n);
    CHECK(g_p[0].repeat == 5 && g_p[1].repeat == 5, "repeat %u/%u, want 5",
          g_p[0].repeat, g_p[1].repeat);
    CHECK(strcmp(g_p[0].text,
                 "\xe2\x86\x92 #54 STATUS u1 THE_FUJI FA GET WIFISTATUS \xc3\x97" "5") == 0,
          "folded request text \"%s\"", g_p[0].text);
    CHECK(strcmp(g_p[1].text, "\xe2\x86\x90 #54 OK 2 B \xc3\x97" "5") == 0,
          "folded reply text \"%s\"", g_p[1].text);
    CHECK(g_p[1].data[0] == 0x54, "folded reply does not carry the newest bytes");
    serial = g_p[1].serial;

    /* The same request with a different answer starts a new pair... */
    request(t, 0x55, 0, 1, 0xFA, -1);
    reply(t, 0x55, 0, 3);
    n = copy(t);
    CHECK(n == 4, "a changed reply folded (%d packets, want 4)", n);
    CHECK(g_p[2].repeat == 1 && g_p[3].repeat == 1, "new pair repeat %u/%u",
          g_p[2].repeat, g_p[3].repeat);
    CHECK(g_p[3].serial > serial, "serial did not rise");

    /* ...which then folds in its own right, and a timeout never does. */
    request(t, 0x56, 0, 1, 0xFA, -1);
    reply(t, 0x56, 0, 3);
    request(t, 0x57, 0, 1, 0xFA, -1);
    sptrace_feed(t, 2, 0x57, NULL, 0);
    n = copy(t);
    CHECK(n == 6, "after a timeout: %d packets, want 6", n);
    CHECK(g_p[3].repeat == 2, "second pair repeat %u, want 2", g_p[3].repeat);
    CHECK(has(g_p[4].text, "#57 STATUS") && has(g_p[5].text, "TIMEOUT"),
          "held request lost: \"%s\" \"%s\"", g_p[4].text, g_p[5].text);

    /* A request that repeats the last one, then something else first: the
     * held request still lands, in order. */
    request(t, 0x58, 0, 1, 0xFA, -1);
    reply(t, 0x58, 0, 3);
    request(t, 0x59, 0, 1, 0xFA, -1);       /* held */
    request(t, 0x5A, 5, 3, 0, -1);
    n = copy(t);
    CHECK(has(g_p[n - 2].text, "#59 STATUS") && has(g_p[n - 1].text, "#5A INIT"),
          "held request out of order: \"%s\" \"%s\"", g_p[n - 2].text,
          g_p[n - 1].text);

    /* ---- the ring keeps the newest 512 ----------------------------------- */
    sptrace_clear(t);
    for (i = 0; i < 600; i++)
        request(t, (uint8_t)i, 5, (uint8_t)(i % 7), 0, -1);
    n = copy(t);
    CHECK(n == SPTRACE_MAX, "ring holds %d, want %d", n, SPTRACE_MAX);
    CHECK(g_p[0].data[0] == (uint8_t)88 && g_p[n - 1].data[0] == (uint8_t)599,
          "ring kept #%02X..#%02X", g_p[0].data[0], g_p[n - 1].data[0]);
    CHECK(sptrace_copy(t, g_p, 3) == 3 && g_p[2].data[0] == (uint8_t)599,
          "a short copy is not the newest");

    sptrace_free(t);
    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("sptrace: decode, fold, interleave and ring all behave\n");
    return 0;
}
