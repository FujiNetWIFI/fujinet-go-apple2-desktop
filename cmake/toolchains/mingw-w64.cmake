# Cross-compile the Windows frontend from Linux with mingw-w64.
#
# The maintainer has no Windows machine, so this is how Windows changes get
# compiled and linked before CI (which builds natively under MSYS2/UCRT64)
# ever sees them. Wine can then run the result for a smoke test.
#
# This matters more here than it did for the ADAM target: neither AppleWin's
# libretro frontend nor FujiNet's IWM device tree had ever been built for
# MinGW before this repository did it, so the Windows path is the one most
# likely to break on an innocent-looking change.
#
#   # once: SDL3 for the cross target
#   git clone --depth 1 --branch release-3.4.12 \
#       https://github.com/libsdl-org/SDL.git /tmp/sdl3-src
#   cmake -S /tmp/sdl3-src -B /tmp/sdl3-src/build -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_INSTALL_PREFIX=/tmp/sdl3-mingw \
#       -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF
#   cmake --build /tmp/sdl3-src/build && cmake --install /tmp/sdl3-src/build
#
#   # zlib for the cross target (AppleWin's minizip wants it). Note the
#   # rename: the build links -lz, and zlib's own CMake installs libzlibstatic.
#   git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git \
#       /tmp/zlib-src
#   cmake -S /tmp/zlib-src -B /tmp/zlib-src/build -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_INSTALL_PREFIX=/tmp/win-deps -DCMAKE_BUILD_TYPE=Release
#   cmake --build /tmp/zlib-src/build && cmake --install /tmp/zlib-src/build
#   cp /tmp/win-deps/lib/libzlibstatic.a /tmp/win-deps/lib/libz.a
#
#   # AppleWin needs Boost headers and the cross sysroot has none. They are
#   # header-only, so the host's copy is fine -- but it must be reached
#   # through a directory that holds NOTHING ELSE. Passing /usr/include
#   # directly puts host glibc on the include path as -isystem, and the build
#   # dies on conflicting ssize_t/time_t between glibc and mingw's corecrt.
#   mkdir -p /tmp/win-boost && ln -sfn /usr/include/boost /tmp/win-boost/boost
#
#   cmake -B build-win -G Ninja -DWITH_FUJINET=OFF \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_PREFIX_PATH=/tmp/sdl3-mingw \
#       -DZLIB_LIBRARY=/tmp/win-deps/lib/libz.a \
#       -DZLIB_INCLUDE_DIR=/tmp/win-deps/include \
#       -DAPPLE2_BOOST_INCLUDE_DIR=/tmp/win-boost
#   cmake --build build-win
#
# For the full app *including* fujinet.dll, FujiNet's own dependencies have
# to be cross-built too (MSYS2 provides them as packages; here they are
# built from source into one prefix, say /tmp/win-deps):
#
#   expat    -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_{TOOLS,EXAMPLES,TESTS}=OFF
#   zlib     (then: cp libzlibstatic.a libz.a -- fujinet links -lz)
#   mbedTLS  3.6.5, MBEDTLS_THREADING_C/PTHREAD enabled in
#            include/mbedtls/mbedtls_config.h, USE_STATIC_MBEDTLS_LIBRARY=ON
#   openssl  ./Configure mingw64 --cross-compile-prefix=x86_64-w64-mingw32- \
#            no-shared no-tests no-apps --prefix=/tmp/win-deps
#
# each configured with -DCMAKE_TOOLCHAIN_FILE pointing here, then:
#
#   export MBEDTLS_ROOT_DIR=/tmp/win-deps
#   export FUJINET_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/mingw-w64.cmake \
#       -DCMAKE_PREFIX_PATH=/tmp/win-deps -DOPENSSL_ROOT_DIR=/tmp/win-deps \
#       -DOPENSSL_USE_STATIC_LIBS=ON -DZLIB_LIBRARY=/tmp/win-deps/lib/libz.a \
#       -DZLIB_INCLUDE_DIR=/tmp/win-deps/include \
#       -DCMAKE_SHARED_LINKER_FLAGS=-L/tmp/win-deps/lib"
#
# Running it under wine wants the runtime beside the exe, exactly like the
# shipped folder, plus the MinGW DLLs the *test* binaries link:
#
#   cp tools/fujinet/work/out/fujinet.dll build-win/frontends/windows/
#   cp -r tools/fujinet/work/out/{data,SD} tools/fujinet/work/out/fnconfig.ini \
#       build-win/frontends/windows/fujinet/
#   WINEPATH='Z:\usr\x86_64-w64-mingw32\bin' wine \
#       build-win/frontends/windows/fujinet-go-apple2-windows.exe
#
# Note that a cross build overwrites tools/fujinet/work/out with the Windows
# runtime; rebuild the host one afterwards (the build directory itself is
# reset automatically when the target changes).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Search only the cross sysroot and whatever prefixes the caller named, so
# host headers and libraries cannot leak into a Windows build. (Without
# this, a find_package can report "found version 1.3.2" from /usr/include
# and then fail on the missing library, which is a confusing way to learn
# that a dependency has not been cross-built yet.)
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX} ${CMAKE_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
