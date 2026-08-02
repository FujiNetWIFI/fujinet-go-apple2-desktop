cmake_minimum_required(VERSION 3.20)
# The version lives here and nowhere else: the About boxes, the macOS bundle
# and the release workflow all take it from PROJECT_VERSION, and cutting a
# tag checks that the tag agrees with it (see "Cutting a release" in the
# README).
project(fujinet-go-apple2-desktop VERSION 0.2.4 LANGUAGES C CXX)

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
set(FRONTEND "all" CACHE STRING
    "Frontend to build: gnome, kde, macos, windows, all, none")
option(WITH_WEBVIEW "Embed the FujiNet web UI (WebKitGTK / QtWebEngine)" ON)

# The Apple II monitor/Applesoft/Disk ][ ROMs are Apple copyrighted firmware
# and are NOT freely licensed. AppleWin's own `apple2roms` resource target
# compiles them into the binary (xxd -i), which is what makes the machine boot
# out of the box. AppleWin itself has shipped this way for over a decade of
# constant public use; this project follows that precedent, including in
# published releases. See COMPLIANCE.md.
#
#   ON  (default) -- embed, as AppleWin itself and the release job do.
#   OFF           -- build ROM-less; the user supplies ROMs through the
#                    "Import System ROMs..." path. For anyone (e.g. a Linux
#                    distribution) who cannot embed Apple's firmware under
#                    their own packaging policy.
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

# The ROM-less claim, checked on every binary built with this option off.
#
# This used to run against the boot_smoke test binary alone. That is a poor
# stand-in: the thing whose contents matter is the executable a user
# downloads, and it links differently. It also hid a bug -- the probe slice
# for one ROM was 64 bytes of 0xFF padding, which occurs in almost any large
# binary but not in the small test one, so the test would have reported a
# false leak the moment it was pointed anywhere real.
if(NOT WITH_APPLE_ROMS)
  set(APPLE2_ROM_CHECK_TARGETS boot_smoke)
  foreach(_fe gnome kde macos windows)
    if(TARGET fujinet-go-apple2-${_fe})
      list(APPEND APPLE2_ROM_CHECK_TARGETS fujinet-go-apple2-${_fe})
    endif()
  endforeach()
  foreach(_t IN LISTS APPLE2_ROM_CHECK_TARGETS)
    add_test(NAME no_embedded_roms_${_t}
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_SOURCE_DIR}/core/tests/no_embedded_roms.py"
                     "$<TARGET_FILE:${_t}>"
                     "${CMAKE_SOURCE_DIR}/core/applewin-generated/resource")
  endforeach()
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

# ---- packaging --------------------------------------------------------
#
# One package per frontend, not one package for both: GNOME (GTK4/
# libadwaita) and KDE (Qt6) pull in entirely different runtime library
# stacks, and a user who wants one should not be made to depend on the
# other. So this expects FRONTEND=gnome or FRONTEND=kde -- exactly the same
# per-frontend split the flatpak manifests already use -- and names the
# package accordingly; FRONTEND=all (the default) still configures fine for
# ordinary development builds, it just packages under a name that says so
# rather than silently mixing both frontends' files into one archive.
if(FRONTEND STREQUAL "gnome")
  set(CPACK_PACKAGE_NAME "fujinet-go-apple2-gnome")
elseif(FRONTEND STREQUAL "kde")
  set(CPACK_PACKAGE_NAME "fujinet-go-apple2-kde")
else()
  set(CPACK_PACKAGE_NAME "fujinet-go-apple2")
endif()

set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "FujiNet")
set(CPACK_PACKAGE_CONTACT "https://github.com/FujiNetWIFI/fujinet-go-apple2-desktop/issues")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://fujinet.online/")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Self-contained Apple II with built-in FujiNet")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;RPM;TGZ")

  # Auto-detected from the actual linked libraries (dpkg-shlibdeps /
  # rpmbuild's own dependency generator) rather than hand-listed: GTK4 vs
  # Qt6 alone makes one fixed list wrong for whichever frontend was not
  # built, and a hand-list silently goes stale the moment a dependency's
  # package name changes across distributions.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")

  set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
  set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
endif()

# CPACK_PACKAGE_FILE_NAME defaults to <name>-<version>-<system>[-<arch>];
# left as-is except spelling out the generator explicitly is unnecessary --
# the default already includes it via the file extension.
include(CPack)
