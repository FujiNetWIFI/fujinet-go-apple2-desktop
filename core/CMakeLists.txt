# libapple2session: the toolkit-agnostic session core. Compiles the staged
# AppleWin tree (like the Android app's cpp-generated tree) plus the desktop
# session/audio/gamepad/FujiNet/debugger layers.

include(GNUInstallDirs)

set(AW "${CMAKE_SOURCE_DIR}/core/applewin-generated")

if(NOT EXISTS "${AW}/source/frontends/libretro/libretro.cpp")
  message(FATAL_ERROR
    "Staged AppleWin tree is missing. cmake/StageAppleWin.cmake should have "
    "produced it; re-run with -DAPPLEWIN_RESTAGE=ON.")
endif()

find_package(SDL3 CONFIG REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- globals AppleWin's top-level CMakeLists would normally establish -------
# It assumes it is the top-level project; we add it as a subdirectory, so the
# settings it expects to be ambient have to be established here.

# Read by the staged source/CMakeLists.txt (see
# tools/applewin/patch-staged-tree.py) to skip libslirp/libpcap detection.
# FujiNet does not use the emulated Uthernet card, and linking the host's
# libslirp would break the flatpak build and the macOS/Windows
# self-containment checks. `appleii` falls back to its dummy network backend.
set(FUJINET_GO_DESKTOP ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# AppleWin is a former Windows project: it uses _DEBUG, not NDEBUG.
add_compile_definitions("$<$<CONFIG:DEBUG>:_DEBUG>")
# Disable the VICE Z80's x86-32 regparm calling-convention optimisation: on
# the 32-bit x86 ABI it turns REGPARM1/2 into __attribute__((regparm(n))), and
# AppleWin declares some Uthernet handlers without the macro and defines them
# with it -- a hard error on x86. The opt is pure micro-tuning, so disabling
# it is correctness-neutral (and a no-op on other ABIs).
add_compile_definitions(NO_REGPARM)
add_compile_options(-Wno-switch -Wno-multichar -Wno-psabi)
# AppleWin sources include their own headers relative to source/.
include_directories("${AW}/source")

# zlib: resolved here, in the scope that is the *parent* of every AppleWin
# subdirectory, because sibling subdirectories do not see each other's
# find_package results.
find_package(ZLIB REQUIRED)

# Boost is header-only for AppleWin's use (property_tree / algorithm-string /
# multi_array in common2). Setting Boost_INCLUDE_DIRS here makes the staged
# CMakeLists skip their own find_package(Boost).
# Located with find_path rather than find_package(Boost): only the headers are
# needed, and CMake 4 removed the FindBoost module (CMP0167) in favour of
# Boost's own config package, which a headers-only install does not ship.
if(DEFINED APPLE2_BOOST_INCLUDE_DIR)
  set(Boost_INCLUDE_DIRS "${APPLE2_BOOST_INCLUDE_DIR}")
else()
  find_path(APPLE2_BOOST_INCLUDE NAMES boost/version.hpp)
  if(NOT APPLE2_BOOST_INCLUDE)
    message(FATAL_ERROR
      "Boost headers (boost/version.hpp) not found. Install the Boost "
      "development headers (they are header-only here -- no compiled Boost "
      "libraries are used) or pass -DAPPLE2_BOOST_INCLUDE_DIR=/path/to/include.")
  endif()
  set(Boost_INCLUDE_DIRS "${APPLE2_BOOST_INCLUDE}")
endif()
message(STATUS "AppleWin: using Boost headers from ${Boost_INCLUDE_DIRS}")

# Put Boost on the include path for EVERY AppleWin subdirectory, not just the
# two that ask for it: frontends/libretro/retroregistry.cpp includes
# common2/ptreeregistry.h, which includes boost/property_tree, but the
# libretro target never adds Boost_INCLUDE_DIRS. That goes unnoticed wherever
# Boost lives in a default include path (Linux /usr/include, MSYS2
# /ucrt64/include) and fails on macOS, where Homebrew puts it under
# /opt/homebrew.
#
# SYSTEM matters: CMake drops a directory it considers an implicit system
# include, so this is a no-op on the platforms where it would have been
# redundant, and only takes effect where it is actually needed.
include_directories(SYSTEM ${Boost_INCLUDE_DIRS})

# How the staged resource/CMakeResources.cmake embeds ROMs and images. It
# shells out to `xxd -i` upstream; the staging step rewrites that to use this,
# because xxd is missing from the flatpak SDKs and from a default MSYS2
# install, and the shell redirection it used needs a shell.
set(APPLE2_XXD_I_COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/applewin/xxd-i.py")

# ---- AppleWin component libraries, in dependency order ---------------------
add_subdirectory("${AW}/libyaml"                     applewin-libyaml)
add_subdirectory("${AW}/minizip"                     applewin-minizip)
# source/linux/libwindows is the Win32-on-POSIX shim: it exists to provide
# the Win32 APIs AppleWin was written against on platforms that lack them.
# On Windows the real ones are there, and AppleWin's own CMakeLists guards
# every use of this target with `if (NOT WIN32)` -- building it there fails
# outright (its fileapi.cpp calls realpath()).
if(NOT WIN32)
  add_subdirectory("${AW}/source/linux/libwindows"   applewin-libwindows)
endif()
add_subdirectory("${AW}/source"                      applewin-source)

# ---- the Apple II system ROMs ----------------------------------------------
# AppleWin's resource/ target compiles the ROM images into the binary with
# xxd -i. common2 links `apple2roms` unconditionally, so the target has to
# exist either way -- what changes is whether it carries Apple's bytes.
# See COMPLIANCE.md.
if(WITH_APPLE_ROMS)
  add_subdirectory("${AW}/resource"                  applewin-resource)
else()
  # Same map, filled from ROM files in the user's ROM directory at first use.
  # spoverslip.bin (the FujiNet SmartPort card firmware, not Apple's) and the
  # debugger font stay embedded, so FujiNet still works without any ROMs.
  set(ROM_LOADER_CPP "${CMAKE_CURRENT_BINARY_DIR}/apple2roms_loader.cpp")
  set(ROM_LOADER_H   "${CMAKE_CURRENT_BINARY_DIR}/apple2roms_data.h")
  set(ROM_EMBED_LIST "${CMAKE_CURRENT_BINARY_DIR}/apple2roms_embedded.txt")
  add_custom_command(
    OUTPUT "${ROM_LOADER_CPP}" "${ROM_LOADER_H}" "${ROM_EMBED_LIST}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/applewin/generate-rom-loader.py"
            "${AW}/resource/CMakeLists.txt"
            "${ROM_LOADER_CPP}" "${ROM_LOADER_H}" "${ROM_EMBED_LIST}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/applewin/generate-rom-loader.py"
            "${AW}/resource/CMakeLists.txt"
    COMMENT "Generating the external ROM loader (WITH_APPLE_ROMS=OFF)"
    VERBATIM)

  # Embed the ones we keep exactly as add_resources() would, so the symbol
  # names the generated loader declares line up.
  # Run the generator at configure time too, so the embed list is known now.
  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/applewin/generate-rom-loader.py"
            "${AW}/resource/CMakeLists.txt"
            "${ROM_LOADER_CPP}" "${ROM_LOADER_H}" "${ROM_EMBED_LIST}"
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "generate-rom-loader.py failed")
  endif()
  file(STRINGS "${ROM_EMBED_LIST}" ROM_EMBED_FILES)

  set(ROM_EMBED_SRC "")
  foreach(_res IN LISTS ROM_EMBED_FILES)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_res}.cpp")
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND ${APPLE2_XXD_I_COMMAND} "${_res}" "${_out}"
      WORKING_DIRECTORY "${AW}/resource"
      DEPENDS "${AW}/resource/${_res}"
      COMMENT "Embedding resource: ${_res}"
      VERBATIM)
    list(APPEND ROM_EMBED_SRC "${_out}")
  endforeach()

  add_library(apple2roms STATIC "${ROM_LOADER_CPP}" "${ROM_LOADER_H}" ${ROM_EMBED_SRC})
  target_include_directories(apple2roms PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
  message(STATUS
    "WITH_APPLE_ROMS=OFF: the Apple II ROMs are NOT embedded; the app reads "
    "them from the user's ROM directory at run time")
endif()

add_subdirectory("${AW}/source/frontends/common2"    applewin-common2)
add_subdirectory("${AW}/source/frontends/libretro"   applewin-libretro)

# ---- the session library ---------------------------------------------------
add_library(apple2session STATIC
  applewin/apple2_host.cpp
  src/session.c
  src/settings.c
  src/paths.c
  src/input_map.c
  src/audio_sdl.c
  src/gamepad_sdl.c
  src/fujinet_runtime.c
  debugger/m6502dasm.c
  debugger/debugger.c
)

target_include_directories(apple2session
  PUBLIC include applewin
  PRIVATE src debugger
          "${AW}/source"
          "${AW}/source/frontends/libretro/libretro-common/include"
)
# -Wno-format-truncation: the path layer composes APPLE2_PATH_MAX-sized
# buffers from other max-sized buffers; snprintf truncation is the intended
# clamp there, not a bug.
target_compile_options(apple2session PRIVATE -Wall -Wextra -Wno-format-truncation)
# apple2_host.cpp is the one TU that includes AppleWin's headers, and those
# headers are not -Wall -Wextra clean (member-init order, unused parameters in
# the Card base class). They are upstream's to fix; silencing them only here
# keeps our own warnings visible instead of drowned.
set_source_files_properties(applewin/apple2_host.cpp PROPERTIES
  COMPILE_OPTIONS "-Wno-reorder;-Wno-unused-parameter")
target_compile_definitions(apple2session PRIVATE
  _GNU_SOURCE
  APPLE2_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  # AppleWin's debug symbol tables, staged into the tree by StageAppleWin.
  APPLE2_SOURCE_SYMBOLS_DIR="${AW}/bin"
  APPLE2_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-apple2"
  APPLE2_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-apple2"
)

# Referencing retro_* directly pulls in the libretro objects and transitively
# appleii / common2 / libwindows / yaml / minizip / apple2roms / zlib.
target_link_libraries(apple2session PUBLIC
  applewin_libretro
  ZLIB::ZLIB
  SDL3::SDL3
  Threads::Threads
)
if(WIN32)
  target_link_libraries(apple2session PUBLIC ws2_32)
else()
  target_link_libraries(apple2session PUBLIC dl m)
endif()

# The debugger reads AppleWin's symbol tables at run time; ship them.
install(FILES "${AW}/bin/APPLE2E.SYM" "${AW}/bin/A2_BASIC.SYM"
              "${AW}/bin/A2_DOS33.SYM2"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fujinet-go-apple2/symbols)

# ---- tests -----------------------------------------------------------------
add_executable(input_test tests/input_test.c)
target_link_libraries(input_test apple2session)

add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke apple2session)

add_executable(debug_seam_test tests/debug_seam_test.c)
target_link_libraries(debug_seam_test apple2session)

add_executable(debug_engine_test tests/debug_engine_test.c)
target_link_libraries(debug_engine_test apple2session)

add_executable(dasm_test tests/dasm_test.c)
target_link_libraries(dasm_test apple2session)
target_include_directories(dasm_test PRIVATE debugger)

add_test(NAME input COMMAND input_test)
add_test(NAME dasm COMMAND dasm_test)
add_test(NAME boot_smoke COMMAND boot_smoke)
add_test(NAME debug_seam COMMAND debug_seam_test)
add_test(NAME debug_engine COMMAND debug_engine_test)
set_tests_properties(boot_smoke debug_seam debug_engine
                     PROPERTIES SKIP_RETURN_CODE 77)

# Makes the COMPLIANCE.md claim checkable: a ROM-less build must not have any
# Apple (or third-party card) firmware compiled into it. Only meaningful for
# the configuration it describes.
if(NOT WITH_APPLE_ROMS)
  add_test(NAME no_embedded_roms
           COMMAND "${Python3_EXECUTABLE}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tests/no_embedded_roms.py"
                   "$<TARGET_FILE:boot_smoke>"
                   "${AW}/resource")
endif()
