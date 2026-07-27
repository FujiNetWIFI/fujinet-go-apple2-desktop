# Provide the AppleWin sources (see cmake/Dependencies.cmake) and stage the
# subset we build into core/applewin-generated, which is what
# core/CMakeLists.txt compiles.
#
# The copy is done in CMake rather than by a shell script: MSYS2/MinGW
# configures cannot execute a .sh through execute_process ("inappropriate
# file type or format"). Unlike adamcore, staging AppleWin is not a pure copy
# -- a handful of idempotent source edits let the upstream tree build as a
# *subdirectory* of ours, and they live in tools/applewin/patch-staged-tree.py
# because a native Windows python IS directly invocable where a .sh is not.
#
# Staging is automatic: it runs when the staged tree is missing, when the
# source checkout has moved to a different commit, when the patch script
# changes, or on demand with -DAPPLEWIN_RESTAGE=ON (which is also how to pick
# up uncommitted edits in a working checkout pointed at by APPLEWIN_SRC).

set(APPLEWIN_GEN "${CMAKE_SOURCE_DIR}/core/applewin-generated")
set(APPLEWIN_PATCH_SCRIPT "${CMAKE_SOURCE_DIR}/tools/applewin/patch-staged-tree.py")

option(APPLEWIN_RESTAGE "Re-stage AppleWin sources from the source checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

apple2_provide_dependency(
  NAME AppleWin
  PATH third_party/applewin
  URL "${APPLEWIN_URL}"
  COMMIT "${APPLEWIN_COMMIT}"
  SENTINEL source/frontends/libretro/libretro.cpp
  OVERRIDE APPLEWIN_SRC
  RESULT APPLEWIN_DIR)

# The subset the libretro build needs. The other desktop frontends (sdl, qt,
# ncurses), the tests, docs and web help are deliberately excluded.
#   bin/ holds APPLE2E.SYM, A2_BASIC.SYM and A2_DOS33.SYM2, which a POST_BUILD
#   step in source/ copies out and the debugger's symbol table reads.
set(APPLEWIN_STAGE_DIRS source resource libyaml minizip bin)

# What the staged tree was made from: the source commit (or the source path,
# for a checkout with no git metadata) plus a hash of the patch script, so
# that editing a transform re-stages rather than silently leaving the old
# result in place.
set(_applewin_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${APPLEWIN_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _applewin_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _applewin_head)
  set(_applewin_head "${APPLEWIN_DIR}")
endif()
file(SHA256 "${APPLEWIN_PATCH_SCRIPT}" _applewin_patch_hash)
set(_applewin_want "${_applewin_head} ${_applewin_patch_hash}")

set(_applewin_staged "")
if(EXISTS "${APPLEWIN_GEN}/.source-info")
  file(READ "${APPLEWIN_GEN}/.source-info" _applewin_staged)
  string(STRIP "${_applewin_staged}" _applewin_staged)
endif()

set(_applewin_sentinel "${APPLEWIN_GEN}/source/frontends/libretro/libretro.cpp")

if(APPLEWIN_RESTAGE OR NOT EXISTS "${_applewin_sentinel}"
   OR NOT _applewin_staged STREQUAL _applewin_want)
  message(STATUS "Staging AppleWin sources from ${APPLEWIN_DIR}")
  file(REMOVE_RECURSE "${APPLEWIN_GEN}")
  file(MAKE_DIRECTORY "${APPLEWIN_GEN}")

  set(_applewin_srcs "")
  foreach(_d IN LISTS APPLEWIN_STAGE_DIRS)
    if(NOT IS_DIRECTORY "${APPLEWIN_DIR}/${_d}")
      message(FATAL_ERROR
        "AppleWin checkout ${APPLEWIN_DIR} is missing ${_d}/ "
        "(is this the FujiNetWIFI 'linux' branch?)")
    endif()
    list(APPEND _applewin_srcs "${APPLEWIN_DIR}/${_d}")
  endforeach()

  # A working checkout usually carries stale in-tree CMake output (the
  # AppleWin build.sh configures in place); copying it would poison ours.
  file(COPY ${_applewin_srcs}
       DESTINATION "${APPLEWIN_GEN}"
       PATTERN ".git" EXCLUDE
       PATTERN "build" EXCLUDE
       PATTERN "CMakeFiles" EXCLUDE
       PATTERN "CMakeCache.txt" EXCLUDE
       PATTERN "cmake_install.cmake" EXCLUDE
       PATTERN "compile_commands.json" EXCLUDE
       REGEX "\\.(o|a|so|d)$" EXCLUDE)

  if(NOT EXISTS "${_applewin_sentinel}")
    message(FATAL_ERROR "AppleWin staging failed (source: ${APPLEWIN_DIR})")
  endif()

  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${APPLEWIN_PATCH_SCRIPT}" "${APPLEWIN_GEN}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    # Leave nothing half-patched behind: the next configure must re-stage
    # from pristine sources rather than trying to patch a partial tree.
    file(REMOVE_RECURSE "${APPLEWIN_GEN}")
    message(FATAL_ERROR
      "AppleWin staging patches failed. The pinned commit "
      "(${APPLEWIN_COMMIT}) and the anchors in\n"
      "  ${APPLEWIN_PATCH_SCRIPT}\n"
      "have to agree; a drifted checkout is the usual cause.\n${_out}${_err}")
  endif()

  file(WRITE "${APPLEWIN_GEN}/.source-info" "${_applewin_want}\n")
endif()

message(STATUS "AppleWin staged at ${_applewin_head}")
