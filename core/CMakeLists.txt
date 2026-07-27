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

# ---- AppleWin component libraries, in dependency order ---------------------
add_subdirectory("${AW}/libyaml"                     applewin-libyaml)
add_subdirectory("${AW}/minizip"                     applewin-minizip)
add_subdirectory("${AW}/source/linux/libwindows"     applewin-libwindows)
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

  # xxd the two we do embed, exactly as add_resources() would, so the symbol
  # names the generated loader declares line up.
  find_program(XXD_EXECUTABLE xxd)
  if(NOT XXD_EXECUTABLE)
    message(FATAL_ERROR "xxd is required to build with WITH_APPLE_ROMS=OFF")
  endif()
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
      COMMAND "${XXD_EXECUTABLE}" -i "${_res}" > "${_out}"
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
)

target_include_directories(apple2session
  PUBLIC include applewin
  PRIVATE src
          "${AW}/source"
          "${AW}/source/frontends/libretro/libretro-common/include"
)
# -Wno-format-truncation: the path layer composes APPLE2_PATH_MAX-sized
# buffers from other max-sized buffers; snprintf truncation is the intended
# clamp there, not a bug.
target_compile_options(apple2session PRIVATE -Wall -Wextra -Wno-format-truncation)
target_compile_definitions(apple2session PRIVATE
  _GNU_SOURCE
  APPLE2_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
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

# ---- tests -----------------------------------------------------------------
add_executable(input_test tests/input_test.c)
target_link_libraries(input_test apple2session)

add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke apple2session)

add_executable(debug_seam_test tests/debug_seam_test.c)
target_link_libraries(debug_seam_test apple2session)

add_test(NAME input COMMAND input_test)
add_test(NAME boot_smoke COMMAND boot_smoke)
add_test(NAME debug_seam COMMAND debug_seam_test)
set_tests_properties(boot_smoke debug_seam PROPERTIES SKIP_RETURN_CODE 77)

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
