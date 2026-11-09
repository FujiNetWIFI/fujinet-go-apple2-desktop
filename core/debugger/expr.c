/*
 * expr.c -- a recursive-descent evaluator for the grammar in expr.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"

typedef struct {
    const char *p;
    const expr_ctx *ctx;
    char *err;
    int errsz;
    int failed;
} parser;

static void fail(parser *ps, const char *msg)
{
    if (!ps->failed && ps->err && ps->errsz > 0)
        snprintf(ps->err, (size_t)ps->errsz, "%s", msg);
    ps->failed = 1;
}

static void skip(parser *ps)
{
    while (*ps->p && isspace((unsigned char)*ps->p))
        ps->p++;
}

static int accept(parser *ps, const char *tok)
{
    size_t n = strlen(tok);
    skip(ps);
    if (strncmp(ps->p, tok, n) == 0)
    {
        ps->p += n;
        return 1;
    }
    return 0;
}

static long expr_or(parser *ps);

static int is_name_start(char c)
{
    return isalpha((unsigned char)c) || c == '_' || c == '.' || c == '@';
}

static int is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@' || c == '\'';
}

/* C000h: hex digits and a closing h, no label of that name */
static int hexh(const char *s)
{
    size_t n = strlen(s), i;
    if (n < 2 || (s[n - 1] != 'h' && s[n - 1] != 'H'))
        return 0;
    for (i = 0; i + 1 < n; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

static long number(parser *ps)
{
    const char *s = ps->p;
    char *end;
    long v;

    if (*s == '$')
    {
        v = strtol(s + 1, &end, 16);
        if (end == s + 1)
            fail(ps, "bad hex number");
        ps->p = end;
        return v;
    }
    if (*s == '%')
    {
        v = strtol(s + 1, &end, 2);
        if (end == s + 1)
            fail(ps, "bad binary number");
        ps->p = end;
        return v;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        v = strtol(s + 2, &end, 16);
        if (end == s + 2)
            fail(ps, "bad hex number");
        ps->p = end;
        return v;
    }
    /* digits: hex with an h suffix (C000h must start with a digit, 0C000h),
     * else decimal */
    {
        const char *q = s;
        while (isxdigit((unsigned char)*q))
            q++;
        if ((*q == 'h' || *q == 'H') && !is_name_char(q[1]))
        {
            v = strtol(s, NULL, 16);
            ps->p = q + 1;
            return v;
        }
    }
    v = strtol(s, &end, 10);
    ps->p = end;
    return v;
}

static long primary(parser *ps)
{
    long v;

    skip(ps);
    if (accept(ps, "("))
    {
        v = expr_or(ps);
        if (!accept(ps, ")"))
            fail(ps, "missing )");
        return v;
    }
    if (accept(ps, "["))
    {
        long a = expr_or(ps);
        if (!accept(ps, "]"))
            fail(ps, "missing ]");
        return ps->ctx && ps->ctx->read8 ? ps->ctx->read8(ps->ctx->user, (uint16_t)a) : 0;
    }
    if (accept(ps, "{"))
    {
        long a = expr_or(ps);
        if (!accept(ps, "}"))
            fail(ps, "missing }");
        if (!ps->ctx || !ps->ctx->read8)
            return 0;
        return ps->ctx->read8(ps->ctx->user, (uint16_t)a)
             | (ps->ctx->read8(ps->ctx->user, (uint16_t)(a + 1)) << 8);
    }
    if (isdigit((unsigned char)*ps->p) || *ps->p == '$' || *ps->p == '%')
        return number(ps);
    if (is_name_start(*ps->p))
    {
        char name[64];
        int n = 0;
        while (is_name_char(*ps->p) && n + 1 < (int)sizeof name)
            name[n++] = *ps->p++;
        name[n] = '\0';
        if (!ps->ctx)
            return 0;
        if (ps->ctx->name && ps->ctx->name(ps->ctx->user, name, &v))
            return v;
        if (hexh(name))
            return strtol(name, NULL, 16);
        {
            char msg[96];
            snprintf(msg, sizeof msg, "unknown name '%s'", name);
            fail(ps, msg);
        }
        return 0;
    }
    if (*ps->p)
        fail(ps, "unexpected character");
    else
        fail(ps, "expression expected");
    return 0;
}

static long unary(parser *ps)
{
    if (accept(ps, "-"))
        return -unary(ps);
    if (accept(ps, "!"))
        return !unary(ps);
    if (accept(ps, "~"))
        return ~unary(ps);
    if (accept(ps, "+"))
        return unary(ps);
    return primary(ps);
}

static long mul(parser *ps)
{
    long v = unary(ps);
    for (;;)
    {
        if (accept(ps, "*"))
            v *= unary(ps);
        else if (accept(ps, "/"))
        {
            long d = unary(ps);
            if (d == 0)
            {
                fail(ps, "division by zero");
                return 0;
            }
            v /= d;
        }
        else if (accept(ps, "%"))
        {
            long d = unary(ps);
            if (d == 0)
            {
                fail(ps, "division by zero");
                return 0;
            }
            v %= d;
        }
        else
            return v;
    }
}

static long add(parser *ps)
{
    long v = mul(ps);
    for (;;)
    {
        if (accept(ps, "+"))
            v += mul(ps);
        else if (accept(ps, "-"))
            v -= mul(ps);
        else
            return v;
    }
}

static long shift(parser *ps)
{
    long v = add(ps);
    for (;;)
    {
        if (accept(ps, "<<"))
            v <<= add(ps);
        else if (accept(ps, ">>"))
            v >>= add(ps);
        else
            return v;
    }
}

static long rel(parser *ps)
{
    long v = shift(ps);
    for (;;)
    {
        if (accept(ps, "<="))
            v = v <= shift(ps);
        else if (accept(ps, ">="))
            v = v >= shift(ps);
        else if (strncmp((skip(ps), ps->p), "<<", 2) != 0 && accept(ps, "<"))
            v = v < shift(ps);
        else if (strncmp((skip(ps), ps->p), ">>", 2) != 0 && accept(ps, ">"))
            v = v > shift(ps);
        else
            return v;
    }
}

static long eq(parser *ps)
{
    long v = rel(ps);
    for (;;)
    {
        if (accept(ps, "=="))
            v = v == rel(ps);
        else if (accept(ps, "!="))
            v = v != rel(ps);
        else
            return v;
    }
}

static long band(parser *ps)
{
    long v = eq(ps);
    for (;;)
    {
        skip(ps);
        if (ps->p[0] == '&' && ps->p[1] != '&')
        {
            ps->p++;
            v &= eq(ps);
        }
        else
            return v;
    }
}

static long bxor(parser *ps)
{
    long v = band(ps);
    while (accept(ps, "^"))
        v ^= band(ps);
    return v;
}

static long bor(parser *ps)
{
    long v = bxor(ps);
    for (;;)
    {
        skip(ps);
        if (ps->p[0] == '|' && ps->p[1] != '|')
        {
            ps->p++;
            v |= bxor(ps);
        }
        else
            return v;
    }
}

static long land(parser *ps)
{
    long v = bor(ps);
    while (accept(ps, "&&"))
    {
        long r = bor(ps);
        v = v && r;
    }
    return v;
}

static long expr_or(parser *ps)
{
    long v = land(ps);
    while (accept(ps, "||"))
    {
        long r = land(ps);
        v = v || r;
    }
    return v;
}

int expr_eval(const char *text, const expr_ctx *ctx, long *out, char *err, int errsz)
{
    parser ps;
    long v;

    memset(&ps, 0, sizeof ps);
    ps.p = text ? text : "";
    ps.ctx = ctx;
    ps.err = err;
    ps.errsz = errsz;
    if (err && errsz > 0)
        err[0] = '\0';

    v = expr_or(&ps);
    skip(&ps);
    if (!ps.failed && *ps.p)
        fail(&ps, "unexpected text after the expression");
    if (ps.failed)
        return -1;
    if (out)
        *out = v;
    return 0;
}
