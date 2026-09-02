# External source dependencies.
#
# Standard practice for every target: each dependency is a pinned git
# submodule under third_party/, and the build provides it for itself. A plain
# `git clone` of this repository (no --recurse-submodules), GNOME Builder, a
# flatpak build or a source tarball all end up with a usable checkout without
# the developer having to know the dependency exists.
#
# Resolution order for each dependency:
#   1. <NAME>_SRC (cache variable or environment) -- an out-of-tree working
#      checkout, which is how the dependencies are developed in tandem, e.g.
#      cmake -B build -DAPPLEWIN_SRC=~/Workspace/AppleWin
#   2. third_party/<name>, initialising the submodule when this tree is a git
#      checkout.
#   3. A direct clone of the pinned commit, for trees with no git metadata
#      (release tarballs, some IDE source copies).

find_package(Git QUIET)

# Pinned commits, kept in step with the submodule gitlinks (verified below).
#
# AppleWin lives on the FujiNetWIFI fork's `linux` branch: it carries the
# SmartPort-over-SLIP card (source/SmartPortOverSlip.cpp + source/devrelay/)
# that the FujiNet link depends on, which upstream does not have.
set(APPLEWIN_COMMIT "25e8ddd139f9ebe02c55e94463e8b876ddd1afc1")
set(APPLEWIN_URL "https://github.com/FujiNetWIFI/AppleWin")
set(FUJINET_COMMIT "13465cdd044304ff96ad41a5f029087b28bed17f")
set(FUJINET_URL "https://github.com/FujiNetWIFI/fujinet-firmware")

# apple2_provide_dependency(NAME <n> PATH <p> URL <u> COMMIT <sha>
#                           SENTINEL <file> OVERRIDE <VAR> RESULT <out>)
#
# SENTINEL is a path inside the checkout that only exists once the sources are
# really there -- an empty submodule directory is otherwise indistinguishable
# from a populated one.
function(apple2_provide_dependency)
  cmake_parse_arguments(DEP "" "NAME;PATH;URL;COMMIT;SENTINEL;OVERRIDE;RESULT"
                        "" ${ARGN})

  # 1. Explicit override: a checkout the developer maintains themselves.
  set(_override "")
  if(DEFINED ${DEP_OVERRIDE})
    set(_override "${${DEP_OVERRIDE}}")
  elseif(DEFINED ENV{${DEP_OVERRIDE}})
    set(_override "$ENV{${DEP_OVERRIDE}}")
  endif()
  if(_override)
    if(NOT EXISTS "${_override}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_OVERRIDE}=${_override} does not look like a ${DEP_NAME} "
        "checkout (no ${DEP_SENTINEL}).")
    endif()
    message(STATUS "${DEP_NAME}: using ${_override} (${DEP_OVERRIDE})")
    set(${DEP_RESULT} "${_override}" PARENT_SCOPE)
    return()
  endif()

  set(_path "${CMAKE_SOURCE_DIR}/${DEP_PATH}")

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
      message(FATAL_ERROR
        "${DEP_NAME} is missing and git is not installed. Either install git "
        "or unpack ${DEP_URL} (commit ${DEP_COMMIT}) into ${DEP_PATH}.")
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
      # 2. Submodule checkout. --filter=blob:none keeps the fetch to the
      # history the build needs; both AppleWin and fujinet-firmware are large
      # repositories.
      message(STATUS "${DEP_NAME}: fetching submodule ${DEP_PATH}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update --init --filter=blob:none
                -- "${DEP_PATH}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        # Older servers/mirrors may refuse partial clones.
        execute_process(
          COMMAND ${GIT_EXECUTABLE} submodule update --init -- "${DEP_PATH}"
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          RESULT_VARIABLE _rc)
      endif()
    endif()

    if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
      # 3. No git metadata (tarball): clone the pin outright.
      message(STATUS "${DEP_NAME}: cloning ${DEP_URL} @ ${DEP_COMMIT}")
      file(REMOVE_RECURSE "${_path}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none "${DEP_URL}" "${_path}"
        RESULT_VARIABLE _rc)
      if(_rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false checkout
                  --quiet "${DEP_COMMIT}"
          WORKING_DIRECTORY "${_path}"
          RESULT_VARIABLE _rc)
      endif()
    endif()

    if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Fetch it manually with\n"
        "    git submodule update --init ${DEP_PATH}\n"
        "or point ${DEP_OVERRIDE} at an existing checkout.")
    endif()
  endif()

  # Warn when a submodule checkout has drifted from the pin recorded here --
  # both the AppleWin staging patches and the FujiNet source patches are
  # anchored to exact text and fail confusingly against a drifted tree.
  if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${_path}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
      if(_rc EQUAL 0 AND NOT _head STREQUAL DEP_COMMIT)
        message(STATUS
          "${DEP_NAME}: checkout is ${_head}, pinned ${DEP_COMMIT} "
          "(cmake/Dependencies.cmake)")
      endif()
    endif()
  endif()

  message(STATUS "${DEP_NAME}: ${_path}")
  set(${DEP_RESULT} "${_path}" PARENT_SCOPE)
endfunction()
