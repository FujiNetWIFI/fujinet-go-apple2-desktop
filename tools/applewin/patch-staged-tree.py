#!/usr/bin/env python3
"""Apply the desktop build's transforms to a staged AppleWin source tree.

Invoked by cmake/StageAppleWin.cmake after it copies the AppleWin subset into
core/applewin-generated. Ported from the Android app's
tools/applewin/build-applewin-core.sh, which does the same job for its NDK
build; the transforms are the same apart from the slirp/pcap one (see below).

Two properties every transform must keep:

  * Idempotent -- re-running against an already-patched tree is a no-op. The
    staging step re-runs whenever the source commit or this script changes,
    and CMake may re-configure at any time.
  * Anchored to exact text, and a HARD FAILURE when the anchor is missing.
    A silently skipped patch produces a build that links but does the wrong
    thing at runtime, which is far more expensive to find than a failed
    configure. If an anchor stops matching, AppleWin has moved and the pin in
    cmake/Dependencies.cmake needs revisiting along with the patch.

Files are read and written as UTF-8 with surrogateescape and newline="".
Python on Windows otherwise decodes the tree as cp1252 (a stray byte in the
firmware/resource tree kills the build) and rewrites every LF to CRLF on the
way out.
"""

import sys
from pathlib import Path


def fail(msg):
    sys.exit(f"patch-staged-tree.py: {msg}")


def patch(root, rel, transforms):
    p = root / rel
    if not p.is_file():
        fail(f"expected file missing: {rel}")
    text = p.read_text(encoding="utf-8", errors="surrogateescape", newline="")
    for old, new in transforms:
        # Test the REPLACEMENT first, not the anchor.
        #
        # Several transforms here wrap their anchor rather than replacing it
        # (the Boost guard, the slot7 block, the Listener::start() guard), so
        # `old` is still a substring of the patched text. Checking the anchor
        # first would therefore re-apply them on every run -- nesting the
        # Boost if(), duplicating the slot7 registry entry -- and staging
        # re-runs whenever CMake re-configures.
        if new in text:
            continue
        if old not in text:
            fail(f"patch anchor not found in {rel}:\n---\n{old}\n---")
        text = text.replace(old, new, 1)
    p.write_text(text, encoding="utf-8", errors="surrogateescape", newline="")


def apply_all(root):
    # -- 1-5: AppleWin assumes it is the top-level CMake project -------------
    # When we add it as a subdirectory, ${CMAKE_SOURCE_DIR} points at *our*
    # tree. Anchor the two such references to the staged tree instead.
    patch(root, "source/CMakeLists.txt", [
        (
            "include(${CMAKE_SOURCE_DIR}/source/slip.cmake)",
            "include(${CMAKE_CURRENT_SOURCE_DIR}/slip.cmake)",
        ),
        # Skip libslirp / libpcap on the desktop build.
        #
        # The Android script skips these because they do not exist for
        # Android. We skip them for a different reason: a Linux developer
        # almost certainly HAS libslirp installed (it is what you need to
        # build AppleWin natively), so pkg-config resolves it and
        # -lslirp -lglib-2.0 enters the link. That breaks the flatpak build
        # (undeclared), the macOS `otool -L` self-containment check and the
        # Windows import-table check -- all for the emulated Uthernet card,
        # which FujiNet does not use. With both unset, `appleii` falls back to
        # its dummy network backend (linux/duplicates/tfearch.cpp) and
        # Uthernet/slirp2 compile out.
        (
            "if (NOT WIN32)\n  pkg_search_module(SLIRP slirp)",
            "if (NOT WIN32 AND NOT FUJINET_GO_DESKTOP)\n  pkg_search_module(SLIRP slirp)",
        ),
        # pkg-config for zlib does not survive cross-compiling (MinGW) or the
        # macOS SDK; CMake's own module does.
        (
            "pkg_search_module(ZLIB REQUIRED zlib)",
            "find_package(ZLIB REQUIRED)",
        ),
        # Boost is header-only here and core/CMakeLists.txt has already
        # resolved it into Boost_INCLUDE_DIRS.
        (
            "find_package(Boost REQUIRED)",
            "if(NOT DEFINED Boost_INCLUDE_DIRS)\n  find_package(Boost REQUIRED)\nendif()",
        ),
        (
            "${CMAKE_SOURCE_DIR}/bin/APPLE2E.SYM ${CMAKE_SOURCE_DIR}/bin/A2_BASIC.SYM ${CMAKE_SOURCE_DIR}/bin/A2_DOS33.SYM2",
            "${CMAKE_CURRENT_SOURCE_DIR}/../bin/APPLE2E.SYM ${CMAKE_CURRENT_SOURCE_DIR}/../bin/A2_BASIC.SYM ${CMAKE_CURRENT_SOURCE_DIR}/../bin/A2_DOS33.SYM2",
        ),
    ])

    # -- 6: common2 also calls find_package(Boost REQUIRED) ------------------
    patch(root, "source/frontends/common2/CMakeLists.txt", [
        (
            "find_package(Boost REQUIRED)",
            "if(NOT DEFINED Boost_INCLUDE_DIRS)\n  find_package(Boost REQUIRED)\nendif()",
        ),
    ])

    # -- 7: getResourceFolder() must not throw when the path is absent ------
    # It canonicalises <exe dir>/ROOT_PATH, a build-relative path that only
    # exists in an AppleWin build tree. The throwing std::filesystem::canonical
    # overload aborts retro_load_game when it does not. ROMs come from the
    # embedded apple2roms map, not this folder (it only seeds g_sProgramDir for
    # debug symbols and printer output), so fall through to the cwd default.
    patch(root, "source/frontends/common2/gnuframe.cpp", [
        (
            "            const auto root = std::filesystem::canonical(executable.parent_path() / ROOT_PATH);\n"
            "            paths.push_back(root);",
            "            std::error_code ec;\n"
            "            const auto root = std::filesystem::canonical(executable.parent_path() / ROOT_PATH, ec);\n"
            "            if (!ec)\n"
            "            {\n"
            "                paths.push_back(root);\n"
            "            }",
        ),
    ])

    # -- 8-9: the SmartPort-over-SLIP Listener singleton ---------------------
    # GetCommandListener() is a process-global reused across emulator re-inits
    # (the user changing machine type). start() did
    # `listening_thread_ = std::thread(...)`, which std::terminate()s when
    # move-assigned over a still-joinable thread from the previous init ->
    # SIGABRT on the second machine switch. Make start() tear down any prior
    # listener, and stop() join whenever the thread is joinable.
    #
    # This is a genuine upstream bug; send it to FujiNetWIFI/AppleWin.
    patch(root, "source/devrelay/service/Listener.cpp", [
        (
            "void Listener::start()\n"
            "{\n"
            "\tis_listening_ = true;\n",
            "void Listener::start()\n"
            "{\n"
            "\t// [fujinet-go-apple2-desktop] The Listener is a process-global singleton\n"
            "\t// reused across emulator re-inits (machine-type change); tear down any\n"
            "\t// prior listener thread first so the std::thread move-assign can't\n"
            "\t// terminate.\n"
            "\tif (is_listening_ || listening_thread_.joinable())\n"
            "\t{\n"
            "\t\tstop();\n"
            "\t}\n"
            "\tis_listening_ = true;\n",
        ),
        (
            "\tif (is_listening_)\n"
            "\t{\n"
            "\t\t// Stop listener first, otherwise the PC might reboot too fast and be picked up\n"
            "\t\tis_listening_ = false;\n"
            "\t\tLogFileOutput(\"Listener::stop() ... joining listener until it stops\\n\");\n"
            "\t\tlistening_thread_.join();",
            "\tis_listening_ = false;\n"
            "\tif (listening_thread_.joinable())\n"
            "\t{\n"
            "\t\t// Stop listener first, otherwise the PC might reboot too fast and be picked up\n"
            "\t\tLogFileOutput(\"Listener::stop() ... joining listener until it stops\\n\");\n"
            "\t\tlistening_thread_.join();",
        ),
    ])

    # -- 10: build the libretro frontend as a STATIC library -----------------
    # The session calls retro_* directly (there is no RetroArch here), so it
    # links whole into libapple2session -- matching adam's single-core model.
    patch(root, "source/frontends/libretro/CMakeLists.txt", [
        (
            "add_library(applewin_libretro SHARED",
            "add_library(applewin_libretro STATIC",
        ),
    ])

    # -- 11-12: expose the SmartPort-over-SLIP card in the core registry ----
    # Slot 7 is the bootable SmartPort/HDD slot the //e autostart scans before
    # the Disk][ in slot 6, so placing FujiNet there boots its CONFIG directly.
    # The card is also offered in Slot 5. The session selects it through the
    # "applewin_slot7" core option, which makes the core insert
    # CT_SmartPortOverSlip and start its Listener on TCP 1985 for the
    # in-process FujiNet runtime to dial into.
    patch(root, "source/frontends/libretro/retroregistry.cpp", [
        (
            '                    {"SAM/DAC", CT_SAM},\n'
            '                },',
            '                    {"SAM/DAC", CT_SAM},\n'
            '                    {"FujiNet", CT_SmartPortOverSlip},\n'
            '                },',
        ),
        (
            '        {\n'
            '            {\n'
            '                "video_mode",',
            '        {\n'
            '            {\n'
            '                "slot7",\n'
            '                "Card in Slot 7",\n'
            '                CATEGORY_SYSTEM,\n'
            '                {\n'
            '                    {"Empty", CT_Empty},\n'
            '                    {"Hard Disk", CT_GenericHDD},\n'
            '                    {"FujiNet", CT_SmartPortOverSlip},\n'
            '                },\n'
            '            },\n'
            '            "Configuration\\\\Slot 7",\n'
            '            REGVALUE_CARD_TYPE, // reset required\n'
            '        },\n'
            '        {\n'
            '            {\n'
            '                "video_mode",',
        ),
    ])

    # -- 13: sample-doubling bug in the libretro speaker mixer --------------
    # When a generator's ring-buffer Read wraps it returns two segments;
    # writeAudio mixes them with two mixBuffer() calls, but `ptr` is passed by
    # value so the second (wrapped) segment is written back at buffer.data()
    # instead of after the first. mixBuffer does *ptr += ..., so the segments
    # sum on top of each other every ring wrap (~16384 frames) -- an audible
    # ~2.7Hz "gallop" over any tone. Offset the second segment by the first's
    # length.
    #
    # Upstream libretro-AppleWin bug; drop this once it is fixed there.
    patch(root, "source/frontends/libretro/rdirectsound.cpp", [
        (
            "                mixBuffer(generator, lpvAudioPtr1, dwAudioBytes1, ptr);\n"
            "                mixBuffer(generator, lpvAudioPtr2, dwAudioBytes2, ptr);",
            "                mixBuffer(generator, lpvAudioPtr1, dwAudioBytes1, ptr);\n"
            "                mixBuffer(generator, lpvAudioPtr2, dwAudioBytes2,\n"
            "                          ptr + dwAudioBytes1 / sizeof(int16_t));",
        ),
    ])


def main():
    # cmake/StageAppleWin.cmake hashes this file into .source-info (with
    # file(SHA256)) so that editing the transforms above re-stages, not just
    # moving the pin in cmake/Dependencies.cmake.
    if len(sys.argv) != 2:
        fail("usage: patch-staged-tree.py <staged-applewin-root>")

    root = Path(sys.argv[1])
    if not (root / "source/frontends/libretro/libretro.cpp").is_file():
        fail(f"{root} does not look like a staged AppleWin tree")
    apply_all(root)


if __name__ == "__main__":
    main()
