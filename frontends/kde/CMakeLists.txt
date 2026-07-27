# KDE frontend: plain Qt6 Widgets -- deliberately NOT KDE Frameworks. Breeze
# arrives through the platform theme, and staying framework-free keeps this
# frontend reusable (and the future Windows port clean). Found-or-skipped so
# a machine without Qt can still build the core and the GNOME side.

find_package(Qt6 6.4 COMPONENTS Widgets OpenGLWidgets QUIET)

if(NOT Qt6_FOUND)
  message(STATUS "KDE frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

set(CMAKE_AUTOMOC ON)

set(KDE_APP_ID online.fujinet.go.apple2.kde)

add_executable(fujinet-go-apple2-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  SettingsDialog.cpp
  FujiNetWindows.cpp
)

target_link_libraries(fujinet-go-apple2-kde PRIVATE
  apple2session Qt6::Widgets Qt6::OpenGLWidgets)
target_compile_options(fujinet-go-apple2-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-apple2-kde PRIVATE
  APPLE2_ICON_FILE="${CMAKE_SOURCE_DIR}/data/icons/hicolor/256x256/apps/fujinet-go-apple2.png")

if(WITH_WEBVIEW)
  # QtWebEngine is enormous and absent on many systems, so it is never a hard
  # dependency: without it the FujiNet config opens in the system browser.
  find_package(Qt6 6.4 COMPONENTS WebEngineWidgets QUIET)
  if(Qt6WebEngineWidgets_FOUND)
    target_link_libraries(fujinet-go-apple2-kde PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(fujinet-go-apple2-kde PRIVATE HAVE_WEBENGINE)
  else()
    message(STATUS "KDE frontend: QtWebEngine not found -- FujiNet config opens in the browser")
  endif()
endif()

install(TARGETS fujinet-go-apple2-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${KDE_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${KDE_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.metainfo.xml")
endif()
