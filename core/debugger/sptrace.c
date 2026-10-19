/*
 * sptrace -- the debugger's FujiNet trace. See sptrace.h.
 *
 * A request is [seq, command, param count, unit, 0, 0, params...]: STATUS and
 * CONTROL carry their code at [6] (CONTROL's payload length at [11..12]),
 * READ/WRITE BLOCK a 24-bit block number at [6..8], READ/WRITE a byte count
 * at [6..7]. A reply is [seq, status, data...]. Units are named from their
 * STATUS 3 (DIB) replies; on THE_FUJI a STATUS or CONTROL code is a Fuji
 * command, elsewhere a SmartPort one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sptrace.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuji_names.h"

#define ARROW_OUT "\xe2\x86\x92" /* → */
#define ARROW_IN  "\xe2\x86\x90" /* ← */
#define TIMES     "\xc3\x97"     /* × */

/* Longest packet a fold compares in full; a longer one never folds. */
#define FOLD_MAX 1024

enum { CMD_STATUS = 0, CMD_READ_BLOCK, CMD_WRITE_BLOCK, CMD_FORMAT,
       CMD_CONTROL, CMD_INIT, CMD_OPEN, CMD_CLOSE, CMD_READ, CMD_WRITE };

static const char *const k_cmd[] = {
    "STATUS", "READ BLOCK", "WRITE BLOCK", "FORMAT", "CONTROL",
    "INIT", "OPEN", "CLOSE", "READ", "WRITE"
};

typedef struct {
    apple2_fuji_packet p;
    uint8_t seq;
    char body[80];          /* the text after the sequence number */
} entry;

typedef struct {
    int live;
    uint8_t cmd, unit, code;
} pending;

typedef struct {
    uint8_t b[FOLD_MAX];
    size_t n;
} bytes;

struct sptrace {
    pthread_mutex_t mtx;
    entry ring[SPTRACE_MAX];
    int head, count;        /* oldest at head */
    uint64_t serial;
    pending pend[256];      /* outstanding requests by sequence number */
    char unit[256][17];     /* names from DIB replies */
    unsigned requests, replies, timeouts;

    /* The last exchange, when it is the ring's last two entries, and the
     * request that repeats it, held until its reply says whether the whole
     * exchange does. */
    int pair_valid;
    bytes pair_req, pair_rep;
    bytes cur_req;          /* the newest request */
    uint8_t cur_seq;
    int held;
    entry held_req;
};

/* ---- formatting ------------------------------------------------------------ */

static void appendf(char *dst, size_t sz, const char *fmt, ...)
{
    size_t len = strlen(dst);
    va_list ap;
    if (len + 1 >= sz) return;
    va_start(ap, fmt);
    vsnprintf(dst + len, sz - len, fmt, ap);
    va_end(ap);
}

static const char *fuji_name(int id)
{
    size_t i;
    for (i = 0; i < sizeof fuji_command_names / sizeof fuji_command_names[0]; i++)
        if (fuji_command_names[i].id == id) return fuji_command_names[i].name;
    return "";
}

static const char *sp_error(int st)
{
    switch (st) {
    case 0x00: return "OK";
    case 0x01: return "BADCMD";
    case 0x06: return "BUSERR";
    case 0x21: return "BADCTL";
    case 0x22: return "BADCTLPARM";
    case 0x27: return "IOERROR";
    case 0x28: return "NODRIVE";
    case 0x2B: return "NOWRITE";
    case 0x2D: return "BADBLOCK";
    case 0x2E: return "DISKSW";
    case 0x2F: return "OFFLINE";
    case 0x30: return "BADWIFI";
    case 0xFF: return "ENDOFCHAIN";
    default: return NULL;
    }
}

/* A STATUS or CONTROL code by name: Fuji commands on THE_FUJI (bar the two
 * status codes the bus answers itself), SmartPort codes elsewhere, and a
 * network unit's ASCII command letters as they are. */
static void code_text(char *dst, size_t sz, int cmd, int fuji, int code)
{
    static const char *const k_status[] = { "STATUS", "DCB", "NEWLINE", "DIB" };
    static const char *const k_control[] = {
        "RESET", "SET DCB", "SET NEWLINE", "SERVICE INTERRUPT", "EJECT"
    };
    const char *name = "";
    char ch[8];

    if (fuji && !(cmd == CMD_STATUS && (code == 0 || code == 3)))
        name = fuji_name(code);
    else if (cmd == CMD_STATUS && code < 4)
        name = k_status[code];
    else if (cmd == CMD_CONTROL && code < 5)
        name = k_control[code];
    else if (code > 0x20 && code < 0x7F) {
        snprintf(ch, sizeof ch, "'%c'", code);
        name = ch;
    }
    snprintf(dst, sz, "%02X%s%s", code, *name ? " " : "", name);
}

/* The public line: arrow, sequence number, body, and the fold count. */
static void compose(entry *e)
{
    char text[sizeof e->p.text];
    snprintf(text, sizeof text, "%s #%02X %s",
             e->p.to_host ? ARROW_IN : ARROW_OUT, e->seq, e->body);
    if (e->p.repeat > 1)
        appendf(text, sizeof text, " " TIMES "%u", e->p.repeat);
    memcpy(e->p.text, text, sizeof text);
}

static void fill(entry *e, int to_host, uint8_t seq, const uint8_t *buf,
                 size_t len)
{
    memset(e, 0, sizeof *e);
    e->p.to_host = to_host;
    e->p.device = e->p.command = e->p.length = -1;
    e->p.nbytes = (int)len;
    e->p.repeat = 1;
    if (buf && len)
        memcpy(e->p.data, buf, len < sizeof e->p.data ? len : sizeof e->p.data);
    e->seq = seq;
}

static void decode_request(sptrace *t, entry *e, const uint8_t *b, size_t n)
{
    int cmd, unit;
    char code[64];

    if (n < 4) {
        snprintf(e->body, sizeof e->body, "%d B (short)", (int)n);
        return;
    }
    cmd = b[1];
    unit = b[3];
    e->p.device = unit;
    e->p.command = cmd;
    e->p.length = n > 11 ? (int)n - 11 : 0;
    if (cmd < 10)
        snprintf(e->body, sizeof e->body, "%s u%d", k_cmd[cmd], unit);
    else
        snprintf(e->body, sizeof e->body, "CMD %02X u%d", cmd, unit);
    if (t->unit[unit][0])
        appendf(e->body, sizeof e->body, " %s", t->unit[unit]);

    switch (cmd) {
    case CMD_STATUS:
    case CMD_CONTROL:
        if (n < 7) break;
        code_text(code, sizeof code, cmd, strcmp(t->unit[unit], "THE_FUJI") == 0,
                  b[6]);
        appendf(e->body, sizeof e->body, " %s", code);
        if (cmd == CMD_CONTROL && n >= 13 && (b[11] | b[12] << 8))
            appendf(e->body, sizeof e->body, " (%d B)", b[11] | b[12] << 8);
        break;
    case CMD_READ_BLOCK:
    case CMD_WRITE_BLOCK:
        if (n >= 9)
            appendf(e->body, sizeof e->body, " blk %lu",
                    (unsigned long)(b[6] | b[7] << 8 | (unsigned long)b[8] << 16));
        break;
    case CMD_READ:
    case CMD_WRITE:
        if (n >= 8)
            appendf(e->body, sizeof e->body, " (%d B)", b[6] | b[7] << 8);
        break;
    default:
        break;
    }
}

/* A DIB reply: [seq, status, dev status, blocks x3, name length, name x16,
 * type, subtype, version x2]. Names are space-padded to 16. */
static void learn_name(sptrace *t, int unit, const uint8_t *b, size_t n)
{
    char name[17];
    size_t len, i, o = 0;
    if (n < 7) return;
    len = b[6];
    if (len > 16) len = 16;
    if (len > n - 7) len = n - 7;
    for (i = 0; i < len; i++)
        if (b[7 + i] >= 0x20 && b[7 + i] < 0x7F) name[o++] = (char)b[7 + i];
    while (o && name[o - 1] == ' ') o--;
    name[o] = '\0';
    if (o) memcpy(t->unit[unit], name, o + 1);
}

static void decode_reply(sptrace *t, entry *e, const pending *pd,
                         const uint8_t *b, size_t n)
{
    const char *err;
    if (pd->live) e->p.device = pd->unit;
    if (n < 2) {
        snprintf(e->body, sizeof e->body, "%d B (short)", (int)n);
        return;
    }
    e->p.command = b[1];
    e->p.length = (int)n - 2;
    err = sp_error(b[1]);
    if (err)
        snprintf(e->body, sizeof e->body, "%s", err);
    else
        snprintf(e->body, sizeof e->body, "ERR %02X", b[1]);
    if (n > 2)
        appendf(e->body, sizeof e->body, " %d B", (int)n - 2);
    if (pd->live && pd->cmd == CMD_STATUS && pd->code == 3 && b[1] == 0) {
        learn_name(t, pd->unit, b, n);
        if (t->unit[pd->unit][0])
            appendf(e->body, sizeof e->body, " %s", t->unit[pd->unit]);
    }
}

/* ---- the ring --------------------------------------------------------------- */

static entry *at(sptrace *t, int i)
{
    return &t->ring[(t->head + i) % SPTRACE_MAX];
}

static void push(sptrace *t, entry *e)
{
    entry *slot;
    if (t->count == SPTRACE_MAX) {
        t->head = (t->head + 1) % SPTRACE_MAX;
        t->count--;
    }
    slot = at(t, t->count++);
    *slot = *e;
    slot->p.serial = ++t->serial;
    compose(slot);
}

static void keep(bytes *k, const uint8_t *buf, size_t len)
{
    k->n = len <= FOLD_MAX ? len : FOLD_MAX + 1; /* too long: never equal */
    if (len <= FOLD_MAX && len) memcpy(k->b, buf, len);
}

/* Equal past the sequence number. */
static int same(const bytes *k, const uint8_t *buf, size_t len)
{
    return k->n <= FOLD_MAX && k->n == len && len >= 2 &&
           memcmp(k->b + 1, buf + 1, len - 1) == 0;
}

static void flush_held(sptrace *t)
{
    if (!t->held) return;
    t->held = 0;
    t->pair_valid = 0;
    push(t, &t->held_req);
}

/* ---- API -------------------------------------------------------------------- */

sptrace *sptrace_new(void)
{
    sptrace *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    pthread_mutex_init(&t->mtx, NULL);
    return t;
}

void sptrace_free(sptrace *t)
{
    if (!t) return;
    pthread_mutex_destroy(&t->mtx);
    free(t);
}

void sptrace_feed(sptrace *t, int kind, uint8_t seq, const uint8_t *buf,
                  size_t len)
{
    entry e;
    pending pd;

    if (!t) return;
    if (!buf) len = 0;
    pthread_mutex_lock(&t->mtx);

    if (kind == 0) {
        t->requests++;
        flush_held(t);
        fill(&e, 0, seq, buf, len);
        decode_request(t, &e, buf, len);
        memset(&t->pend[seq], 0, sizeof t->pend[seq]);
        if (len >= 4) {
            t->pend[seq].live = 1;
            t->pend[seq].cmd = buf[1];
            t->pend[seq].unit = buf[3];
            t->pend[seq].code = len >= 7 ? buf[6] : 0;
        }
        keep(&t->cur_req, buf, len);
        t->cur_seq = seq;
        if (t->pair_valid && same(&t->pair_req, buf, len)) {
            t->held = 1;
            t->held_req = e;
        } else {
            t->pair_valid = 0;
            push(t, &e);
        }
        pthread_mutex_unlock(&t->mtx);
        return;
    }

    pd = t->pend[seq];
    t->pend[seq].live = 0;
    fill(&e, 1, seq, buf, len);
    if (kind == 1) {
        t->replies++;
        decode_reply(t, &e, &pd, buf, len);
    } else {
        t->timeouts++;
        if (pd.live) e.p.device = pd.unit;
        snprintf(e.body, sizeof e.body, "TIMEOUT");
    }

    /* The held request's reply repeats the last one too: fold the exchange. */
    if (t->held && t->held_req.seq == seq && kind == 1 &&
        same(&t->pair_rep, buf, len) && t->count >= 2) {
        entry *rq = at(t, t->count - 2), *rp = at(t, t->count - 1);
        t->held = 0;
        rq->seq = rp->seq = seq;
        memcpy(rq->p.data, t->held_req.p.data, sizeof rq->p.data);
        memcpy(rp->p.data, e.p.data, sizeof rp->p.data);
        rq->p.repeat++;
        rp->p.repeat++;
        rq->p.serial = ++t->serial;
        rp->p.serial = ++t->serial;
        compose(rq);
        compose(rp);
        pthread_mutex_unlock(&t->mtx);
        return;
    }
    flush_held(t);
    push(t, &e);

    /* A reply right after its request is the exchange the next one may
     * repeat. */
    t->pair_valid = kind == 1 && t->count >= 2 && seq == t->cur_seq &&
                    !at(t, t->count - 2)->p.to_host &&
                    at(t, t->count - 2)->seq == seq;
    if (t->pair_valid) {
        t->pair_req = t->cur_req;
        keep(&t->pair_rep, buf, len);
    }
    pthread_mutex_unlock(&t->mtx);
}

int sptrace_copy(sptrace *t, apple2_fuji_packet *out, int max)
{
    int n, i, start;
    if (!t || !out || max <= 0) return 0;
    pthread_mutex_lock(&t->mtx);
    n = t->count < max ? t->count : max;
    start = t->count - n;
    for (i = 0; i < n; i++)
        out[i] = at(t, start + i)->p;
    pthread_mutex_unlock(&t->mtx);
    return n;
}

void sptrace_clear(sptrace *t)
{
    if (!t) return;
    pthread_mutex_lock(&t->mtx);
    t->head = t->count = 0;
    t->held = 0;
    t->pair_valid = 0;
    t->requests = t->replies = t->timeouts = 0;
    pthread_mutex_unlock(&t->mtx);
}

int sptrace_describe(sptrace *t, char *dst, int dstsz)
{
    int i, named = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!t) return 0;
    pthread_mutex_lock(&t->mtx);
    appendf(dst, (size_t)dstsz,
            "Since the trace was cleared\n"
            "Requests   %u\n"
            "Replies    %u\n"
            "Timeouts   %u\n"
            "\nUnits (named by their DIB replies)\n",
            t->requests, t->replies, t->timeouts);
    for (i = 0; i < 256; i++) {
        if (!t->unit[i][0]) continue;
        appendf(dst, (size_t)dstsz, "%3d  %s\n", i, t->unit[i]);
        named++;
    }
    if (!named)
        appendf(dst, (size_t)dstsz, "(none yet)\n");
    pthread_mutex_unlock(&t->mtx);
    return (int)strlen(dst);
}
