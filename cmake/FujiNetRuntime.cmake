# Build the FujiNet firmware as a shared library and install it alongside the
# app, so that a plain `cmake --build` produces an application with FujiNet
# inside it. The session dlopen's the result and joins it to the emulator over
# loopback TCP 1985 (SmartPort-over-SLIP).
#
# NOTE: the recipe (tools/fujinet/build-fujinet-desktop.sh) lands in milestone
# 3. Until it exists this module disables itself with a warning rather than
# failing the build, so the emulator half can be developed first.

option(WITH_FUJINET "Build and embed the FujiNet runtime" ON)

set(FUJINET_SCRIPT "${CMAKE_SOURCE_DIR}/tools/fujinet/build-fujinet-desktop.sh")

if(WITH_FUJINET AND NOT EXISTS "${FUJINET_SCRIPT}")
  message(WARNING
    "WITH_FUJINET=ON but ${FUJINET_SCRIPT} does not exist yet; building "
    "without FujiNet. The Apple II will boot, but there will be no FujiNet "
    "device on the SmartPort bus.")
  set(WITH_FUJINET OFF)
endif()

if(NOT WITH_FUJINET)
  return()
endif()

message(FATAL_ERROR
  "FujiNetRuntime.cmake: the build recipe exists but this module has not been "
  "written yet. This is a bug -- it should have landed with the script.")
