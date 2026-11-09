/* memtools: search parsing, watch list, trace filters (no ROMs needed). */
#include <stdio.h>
#include <string.h>

#include "apple2memtools.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void)
{
    uint8_t pat[APPLE2SEARCH_MAX], mask;
    int n, flags;
    apple2debug_trace t = { .pc = 0x0803, .len = 3, .bytes = { 0x20, 0x62, 0xFA } }; /* JSR $FA62 */
    apple2debug_trace u = { .pc = 0x9000, .len = 1, .bytes = { 0xEA } };
    char buf[128];

    CHECK(apple2search_parse(APPLE2SEARCH_HEX, "5A 3c,$FF 0x10", pat, &n, &mask, &flags), "hex parses");
    CHECK(n == 4 && pat[0] == 0x5A && pat[1] == 0x3C && pat[2] == 0xFF && pat[3] == 0x10, "hex bytes");
    CHECK(mask == 0xFF && flags == 0, "hex mask/flags");
    CHECK(apple2search_parse(APPLE2SEARCH_HEX, "5", pat, &n, &mask, &flags) && n == 1 && pat[0] == 5, "lone digit");
    CHECK(!apple2search_parse(APPLE2SEARCH_HEX, "zz", pat, &n, &mask, &flags), "bad hex");
    CHECK(!apple2search_parse(APPLE2SEARCH_HEX, "  ", pat, &n, &mask, &flags), "empty hex");
    CHECK(apple2search_parse(APPLE2SEARCH_TEXT, "Ab", pat, &n, &mask, &flags) && n == 2 &&
          pat[0] == 'A' && (flags & APPLE2DEBUG_SEARCH_NOCASE), "text");

    apple2watch_clear();
    CHECK(apple2watch_add("  $4100 ") == 0 && apple2watch_add("score") == 1, "add");
    CHECK(apple2watch_add("   ") == -1, "empty add refused");
    CHECK(apple2watch_count() == 2 && !strcmp(apple2watch_text(0), "$4100"), "text trimmed");
    {
        unsigned g = apple2watch_generation();
        apple2watch_remove(0);
        CHECK(apple2watch_count() == 1 && !strcmp(apple2watch_text(0), "score"), "remove");
        CHECK(apple2watch_generation() != g, "generation bumps");
    }
    apple2watch_clear();
    CHECK(apple2watch_count() == 0, "clear");

    CHECK(apple2debug_trace_match(&t, APPLE2DEBUG_TRACE_ALL, 0, 0), "all");
    CHECK(apple2debug_trace_match(&t, APPLE2DEBUG_TRACE_BRANCHES, 0, 0), "JSR is a branch");
    CHECK(!apple2debug_trace_match(&u, APPLE2DEBUG_TRACE_BRANCHES, 0, 0), "NOP is not");
    CHECK(apple2debug_trace_match(&t, APPLE2DEBUG_TRACE_RANGE, 0x0800, 0x08FF), "in range");
    CHECK(!apple2debug_trace_match(&u, APPLE2DEBUG_TRACE_RANGE, 0x0800, 0x08FF), "out of range");
    CHECK(apple2debug_trace_match(&t, APPLE2DEBUG_TRACE_RANGE, 0x08FF, 0x0800), "range swaps");
    apple2debug_trace_format(&t, buf, sizeof buf);
    CHECK(!strncmp(buf, "0803  JSR $FA62", 15), "trace format");

    printf(fails ? "memtools_test: %d failed\n" : "memtools_test: ok\n", fails);
    return fails != 0;
}
