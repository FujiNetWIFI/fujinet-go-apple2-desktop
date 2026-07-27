# The FujiNet runtime (libfujinet), built from the pinned fujinet-firmware
# checkout and bundled with the app.
#
# This is what makes the desktop app self-contained: the emulator talks to a
# real FujiNet over loopback "Bus over IP", so the firmware has to be built
# and shipped alongside the frontends. The build does it by itself -- the
# sources are provided like any other dependency (cmake/Dependencies.cmake)
# and tools/fujinet/build-fujinet-desktop.sh stages, patches and builds them
# into tools/fujinet/work/out, where both an uninstalled run and the install
# rules pick it up.
#
# The first build takes a few minutes; later ones are incremental. Configure
# with -DWITH_FUJINET=OFF for a bare emulator build.

option(WITH_FUJINET "Build and bundle the FujiNet runtime" ON)

set(FUJINET_OUT "${CMAKE_SOURCE_DIR}/tools/fujinet/work/out")
if(APPLE)
  set(FUJINET_LIB_NAME "libfujinet.dylib")
elseif(WIN32)
  set(FUJINET_LIB_NAME "fujinet.dll")
else()
  set(FUJINET_LIB_NAME "libfujinet.so")
endif()
set(FUJINET_LIB "${FUJINET_OUT}/${FUJINET_LIB_NAME}")

if(WITH_FUJINET)
  apple2_provide_dependency(
    NAME fujinet-firmware
    PATH third_party/fujinet-firmware
    URL "${FUJINET_URL}"
    COMMIT "${FUJINET_COMMIT}"
    SENTINEL build.sh
    OVERRIDE FUJINET_SRC
    RESULT FUJINET_DIR)

  set(FUJINET_SCRIPT "${CMAKE_SOURCE_DIR}/tools/fujinet/build-fujinet-desktop.sh")

  # Always invoke the recipe through bash by name. A native Windows cmake
  # (MSYS2's, which is what builds the Windows app) hands a bare .sh to
  # cmd.exe and gets "inappropriate file type or format"; on Linux and macOS
  # this is what the shebang would have done anyway.
  find_program(BASH_EXECUTABLE bash)
  if(NOT BASH_EXECUTABLE)
    message(FATAL_ERROR
      "bash is required to build the FujiNet runtime (-DWITH_FUJINET=OFF "
      "builds the emulator without it). On Windows, build from an MSYS2 "
      "UCRT64 shell.")
  endif()

  # Re-run the runtime build when the recipe changes or the sources are
  # pointed somewhere else. configure_file() leaves the stamp alone when the
  # content is unchanged, so re-configuring does not rebuild FujiNet.
  set(FUJINET_STAMP "${CMAKE_BINARY_DIR}/fujinet-source.stamp")
  file(WRITE "${CMAKE_BINARY_DIR}/CMakeFiles/fujinet-source.stamp.in"
       "${FUJINET_DIR}\n${FUJINET_COMMIT}\n")
  configure_file("${CMAKE_BINARY_DIR}/CMakeFiles/fujinet-source.stamp.in"
                 "${FUJINET_STAMP}" COPYONLY)

  add_custom_command(
    OUTPUT "${FUJINET_LIB}"
    COMMAND ${CMAKE_COMMAND} -E env "FUJINET_SRC=${FUJINET_DIR}"
            "${BASH_EXECUTABLE}" "${FUJINET_SCRIPT}"
    DEPENDS "${FUJINET_SCRIPT}"
            "${CMAKE_SOURCE_DIR}/tools/fujinet/support/fujinet_desktop_entry.cpp"
            "${FUJINET_STAMP}"
    COMMENT "Building the FujiNet runtime (first build takes a few minutes)"
    USES_TERMINAL
    VERBATIM)
  add_custom_target(fujinet-runtime ALL DEPENDS "${FUJINET_LIB}")

  # Escape hatch for working on the FujiNet sources themselves: always
  # re-stages and rebuilds, whatever the stamps say.
  add_custom_target(fujinet-runtime-refresh
    COMMAND ${CMAKE_COMMAND} -E env "FUJINET_SRC=${FUJINET_DIR}" "FN_REFRESH=1"
            "${BASH_EXECUTABLE}" "${FUJINET_SCRIPT}"
    COMMENT "Re-staging and rebuilding the FujiNet runtime"
    USES_TERMINAL
    VERBATIM)
endif()

# Install the runtime: whatever this build produced, or a tree left by an
# earlier manual run of the script.
if(WITH_FUJINET OR EXISTS "${FUJINET_LIB}")
  if(WIN32)
    # A Windows install is a folder you copy: fujinet.dll sits beside the
    # exe (where the session looks first) and the runtime tree beside that.
    install(FILES "${FUJINET_LIB}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES "${FUJINET_OUT}/fnconfig.ini"
            DESTINATION ${CMAKE_INSTALL_BINDIR}/fujinet)
    install(DIRECTORY "${FUJINET_OUT}/data" "${FUJINET_OUT}/SD"
            DESTINATION ${CMAKE_INSTALL_BINDIR}/fujinet)
  else()
    install(FILES "${FUJINET_LIB}"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/fujinet-go-apple2)
    install(FILES "${FUJINET_OUT}/fnconfig.ini"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/fujinet-go-apple2/fujinet)
    install(DIRECTORY "${FUJINET_OUT}/data" "${FUJINET_OUT}/SD"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/fujinet-go-apple2/fujinet)
  endif()
elseif(NOT WIN32)
  message(STATUS "FujiNet runtime disabled (WITH_FUJINET=OFF); "
                 "the emulator will run without FujiNet")
endif()
