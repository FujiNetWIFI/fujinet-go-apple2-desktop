/*
 * test_tmpdir.h -- a writable directory for mkdtemp() templates.
 *
 * POSIX guarantees /tmp; a native (UCRT64, non-MSYS-runtime) Windows binary
 * has no such path -- mkdtemp("/tmp/...") fails there with ENOENT because
 * "/tmp" itself doesn't exist, not because of anything about the template.
 * %TEMP%/%TMP% are what Windows actually provides.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2_TEST_TMPDIR_H
#define APPLE2_TEST_TMPDIR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void test_tmpl(char *out, size_t outsz, const char *name)
{
    const char *d = getenv("TMPDIR");
    if (!d || !*d) d = getenv("TEMP");
    if (!d || !*d) d = getenv("TMP");
    if (!d || !*d) d = "/tmp";
    size_t n = strlen(d);
    const char *sep = (n > 0 && (d[n - 1] == '/' || d[n - 1] == '\\')) ? "" : "/";
    snprintf(out, outsz, "%s%s%s.XXXXXX", d, sep, name);
}

#endif
