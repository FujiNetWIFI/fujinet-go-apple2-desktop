# GNOME frontend: GTK4 + libadwaita, with an embedded WebKitGTK view of the
# FujiNet web admin when available (WITH_WEBVIEW). Found-or-skipped so lean
# machines can still build the core / KDE side.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ADW IMPORTED_TARGET libadwaita-1>=1.4 gtk4>=4.10)

if(NOT ADW_FOUND)
  message(STATUS "GNOME frontend: libadwaita/gtk4 not found -- skipping")
  return()
endif()

set(GNOME_SOURCES
  main.c
  window.c
  display.c
  prefs.c
  webview.c
  debugger/dbg_window.c
)

set(GNOME_APP_ID online.fujinet.go.apple2.gnome)

add_executable(fujinet-go-apple2-gnome ${GNOME_SOURCES})
target_link_libraries(fujinet-go-apple2-gnome PRIVATE apple2session PkgConfig::ADW)
target_compile_options(fujinet-go-apple2-gnome PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-apple2-gnome PRIVATE
  APPLE2_APP_ID="${GNOME_APP_ID}"
  # Uninstalled runs fall back to the in-tree icon theme (apple2_icon_name).
  APPLE2_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

if(WITH_WEBVIEW)
  pkg_check_modules(WEBKIT IMPORTED_TARGET webkitgtk-6.0)
  if(WEBKIT_FOUND)
    target_link_libraries(fujinet-go-apple2-gnome PRIVATE PkgConfig::WEBKIT)
    target_compile_definitions(fujinet-go-apple2-gnome PRIVATE HAVE_WEBKIT)
  else()
    message(STATUS "GNOME frontend: webkitgtk-6.0 not found -- FujiNet config opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-apple2-gnome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${GNOME_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${GNOME_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Catch a malformed launcher at build time rather than after installing.
find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_gnome
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${GNOME_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_gnome
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${GNOME_APP_ID}.metainfo.xml")
endif()
