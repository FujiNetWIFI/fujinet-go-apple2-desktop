/*
 * Cross-platform dynamic-library seam: dlopen/dlsym on POSIX,
 * LoadLibrary/GetProcAddress on Windows. Kept header-only so the one
 * consumer (fujinet_runtime.c) stays simple.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLE2SESSION_DYNLIB_H
#define APPLE2SESSION_DYNLIB_H

#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)

#include <windows.h>

typedef HMODULE apple2_dynlib;

static inline apple2_dynlib apple2_dynlib_open(const char *path)
{
    /* LOAD_WITH_ALTERED_SEARCH_PATH puts the library's own directory ahead
     * of the process directory when resolving *its* dependencies, so a
     * fujinet.dll installed next to its support DLLs loads wherever it
     * lives. */
    return LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static inline void *apple2_dynlib_sym(apple2_dynlib h, const char *name)
{
    return (void *)(uintptr_t)GetProcAddress(h, name);
}

/* Formats the last load/sym error into buf; returns buf. */
static inline const char *apple2_dynlib_error(char *buf, int buflen)
{
    DWORD e = GetLastError();
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL, e, 0, buf, (DWORD)buflen, NULL))
        snprintf(buf, (size_t)buflen, "error %lu", (unsigned long)e);
    return buf;
}

#else

#include <dlfcn.h>

typedef void *apple2_dynlib;

static inline apple2_dynlib apple2_dynlib_open(const char *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static inline void *apple2_dynlib_sym(apple2_dynlib h, const char *name)
{
    return dlsym(h, name);
}

static inline const char *apple2_dynlib_error(char *buf, int buflen)
{
    const char *e = dlerror();
    snprintf(buf, (size_t)buflen, "%s", e ? e : "(unknown)");
    return buf;
}

#endif

#endif /* APPLE2SESSION_DYNLIB_H */
