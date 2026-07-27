/*
 * FujiNet Go Apple II -- macOS frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include <stdio.h>
#include <string.h>

#include "apple2session.h"

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    @autoreleasepool {
        /* A bundled FujiNet runtime (the build packs libfujinet.dylib into
         * Contents/Frameworks and the pristine runtime tree into
         * Contents/Resources/fujinet) takes priority; without it the session
         * falls back to its default search and, failing that, runs the
         * machine without the FujiNet card. */
        apple2session_paths paths;
        memset(&paths, 0, sizeof(paths));
        NSString *lib = [[[NSBundle mainBundle] privateFrameworksPath]
            stringByAppendingPathComponent:@"libfujinet.dylib"];
        NSString *src = [[[NSBundle mainBundle] resourcePath]
            stringByAppendingPathComponent:@"fujinet"];
        NSFileManager *fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:lib])
            paths.fujinet_lib = lib.UTF8String;
        if ([fm fileExistsAtPath:
                    [src stringByAppendingPathComponent:@"fnconfig.ini"]])
            paths.fujinet_runtime_src = src.UTF8String;

        apple2session *session = apple2session_new(&paths);
        if (!session) {
            fprintf(stderr, "fatal: could not create the session\n");
            return 1;
        }
        apple2session_start_opts opts;
        apple2session_default_opts(session, &opts);
        if (apple2session_start(session, &opts) != 0)
            fprintf(stderr, "session start: %s\n",
                    apple2session_last_error(session));

        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate = [[AppDelegate alloc] initWithSession:session];
        app.delegate = delegate;
        [app run];

        apple2session_free(session);
    }
    return 0;
}
