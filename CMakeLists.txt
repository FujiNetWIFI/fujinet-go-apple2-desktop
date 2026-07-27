cmake_minimum_required(VERSION 3.20)
# The version lives here and nowhere else: the About boxes, the macOS bundle
# and the release workflow all take it from PROJECT_VERSION, and cutting a
# tag checks that the tag agrees with it (see "Cutting a release" in the
# README).
project(fujinet-go-apple2-desktop VERSION 0.1.0 LANGUAGES C CXX)

add_compile_definitions(APPLE2_VERSION_STRING="${PROJECT_VERSION}")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# AppleWin is C++17; the staged tree is compiled as part of this project.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Which native frontend(s) to build. Each is found-or-skipped so a lean
# machine can still build the other one (or just the core + tests).
set(FRONTEND "all" CACHE STRING "Frontend to build: gnome, kde, all, none")
option(WITH_WEBVIEW "Embed the FujiNet web UI (WebKitGTK / QtWebEngine)" ON)

# The Apple II monitor/Applesoft/Disk ][ ROMs are Apple copyrighted firmware
# and are NOT freely licensed. AppleWin's own `apple2roms` resource target
# compiles them into the binary (xxd -i), which is what makes the machine boot
# out of the box -- but it also makes the resulting artifact unsuitable for
# public redistribution. See COMPLIANCE.md.
#
#   ON  (default) -- embed, as the Android app does. Local/development builds.
#   OFF           -- build ROM-less; the user supplies ROMs through the
#                    "Import System ROMs..." path. This is what the release
#                    job builds.
option(WITH_APPLE_ROMS "Embed the Apple II system ROMs (see COMPLIANCE.md)" ON)

# Included here, at the top of the tree, so every subdirectory sees the
# *variables* it defines: GNUInstallDirs caches CMAKE_INSTALL_DATADIR as an
# empty string and only sets its non-cache fallback (share) in the including
# scope. Including it in a subdirectory (as core/ does) therefore leaves the
# frontends with an empty DATADIR -- and "${CMAKE_INSTALL_DATADIR}/applications"
# then resolves to the absolute path /applications, outside the prefix.
include(GNUInstallDirs)

include(cmake/Dependencies.cmake)
include(cmake/StageAppleWin.cmake)

enable_testing()
add_subdirectory(core)

# The FujiNet runtime: built from the pinned firmware sources as part of the
# normal build and installed alongside the app (the session provisions the
# user's runtime tree from the installed copy on first start). Set up before
# the frontends so the macOS bundle can depend on the library it packs.
include(cmake/FujiNetRuntime.cmake)

# Each frontend is native to one platform; with FRONTEND=all (the default)
# only the host's viable frontends are considered, so a Windows/macOS
# configure never descends into the Linux (pkg-config) frontends and vice
# versa. Naming a frontend explicitly still forces just that one.
if(FRONTEND STREQUAL "gnome" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_subdirectory(frontends/gnome)
endif()
if(FRONTEND STREQUAL "kde" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_subdirectory(frontends/kde)
endif()
if(FRONTEND STREQUAL "macos" OR (FRONTEND STREQUAL "all" AND APPLE))
  add_subdirectory(frontends/macos)
endif()
if(FRONTEND STREQUAL "windows" OR (FRONTEND STREQUAL "all" AND WIN32))
  add_subdirectory(frontends/windows)
endif()

# Application icons (rendered from the Android app's launcher art by
# tools/icons/make-icons.py). Desktop environments look the icon up by the
# desktop-entry id, so the same art is installed under each frontend's id --
# at every hicolor size, which is what makes it show up crisply everywhere
# from the 16px window list to the 256px app grid.
set(APPLE2_ICON_SIZES 16 24 32 48 64 128 192 256 512)
set(APPLE2_ICON_IDS online.fujinet.go.apple2.gnome online.fujinet.go.apple2.kde)
foreach(_size IN LISTS APPLE2_ICON_SIZES)
  foreach(_id IN LISTS APPLE2_ICON_IDS)
    install(FILES data/icons/hicolor/${_size}x${_size}/apps/fujinet-go-apple2.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
            RENAME ${_id}.png)
  endforeach()
endforeach()
