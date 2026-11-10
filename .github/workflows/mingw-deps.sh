#!/usr/bin/env bash
# Cross-build the static libraries the Windows build needs (zlib, expat, MbedTLS, OpenSSL for the
# FujiNet runtime, and SDL3 for the app) with MinGW-w64 into <prefix>, and write
# <prefix>/toolchain.cmake. Copied from fujinet-firmware's mingw-deps.sh, plus SDL3. Run on Ubuntu with the
# mingw-w64, cmake, ninja, perl, git and curl packages installed.
set -euo pipefail

PREFIX=${1:?usage: mingw-deps.sh <prefix>}
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
WORK=$PREFIX/src
JOBS=$(nproc)

TRIPLE=x86_64-w64-mingw32
# the posix thread model, which std::thread and std::mutex need
CC_WIN=$TRIPLE-gcc-posix
CXX_WIN=$TRIPLE-g++-posix

ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
EXPAT_SHA256=354552544b8f99012e5062f7d570ec77f14b412a3ff5c7d8d0dae62c0d217c30
MBEDTLS_SHA256=4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8
SDL3_SHA256=f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7 # SDL3-3.4.12.tar.gz
OPENSSL_COMMIT=636dfadc70ce26f2473870570bfd9ec352806b1d # openssl-3.5.0

cat > "$PREFIX/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $CC_WIN)
set(CMAKE_CXX_COMPILER $CXX_WIN)
set(CMAKE_RC_COMPILER $TRIPLE-windres)
set(CMAKE_FIND_ROOT_PATH /usr/$TRIPLE $PREFIX)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# fujinet_pc.cmake overrides CMAKE_EXE_LINKER_FLAGS, so -static and the library path ride here
set(CMAKE_C_STANDARD_LIBRARIES "-L$PREFIX/lib -static -lws2_32 -lcrypt32 -lbcrypt -luuid -lole32 -lcomdlg32" CACHE STRING "")
set(CMAKE_CXX_STANDARD_LIBRARIES "-L$PREFIX/lib -static -lws2_32 -lcrypt32 -lbcrypt -luuid -lole32 -lcomdlg32" CACHE STRING "")
EOF

mkdir -p "$WORK"
cd "$WORK"

fetch() { # <url> <sha256> <file>
    curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum -c -
}

CMAKE_DEP=(-G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$PREFIX/toolchain.cmake"
           "-DCMAKE_INSTALL_PREFIX=$PREFIX")

fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz "$ZLIB_SHA256" zlib.tar.gz
tar -xzf zlib.tar.gz
cmake -S zlib-1.3.1 -B zlib-build "${CMAKE_DEP[@]}"
cmake --build zlib-build -j"$JOBS"
cmake --install zlib-build
# zlib installs the shared import library too; keep the link static
rm -f "$PREFIX"/lib/libzlib.dll.a "$PREFIX"/bin/zlib1.dll
cp "$PREFIX/lib/libzlibstatic.a" "$PREFIX/lib/libz.a"

fetch https://github.com/libexpat/libexpat/releases/download/R_2_7_1/expat-2.7.1.tar.xz "$EXPAT_SHA256" expat.tar.xz
tar -xJf expat.tar.xz
cmake -S expat-2.7.1 -B expat-build "${CMAKE_DEP[@]}" -DEXPAT_SHARED_LIBS=OFF \
    -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
    -DEXPAT_BUILD_DOCS=OFF
cmake --build expat-build -j"$JOBS"
cmake --install expat-build

fetch https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.5/mbedtls-3.6.5.tar.bz2 \
    "$MBEDTLS_SHA256" mbedtls.tar.bz2
tar -xjf mbedtls.tar.bz2
cmake -S mbedtls-3.6.5 -B mbedtls-build "${CMAKE_DEP[@]}" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DMBEDTLS_FATAL_WARNINGS=OFF
cmake --build mbedtls-build -j"$JOBS"
cmake --install mbedtls-build

git init -q openssl-src
git -C openssl-src fetch -q --depth 1 https://github.com/openssl/openssl.git "$OPENSSL_COMMIT"
git -C openssl-src checkout -q FETCH_HEAD
test "$(git -C openssl-src rev-parse HEAD)" = "$OPENSSL_COMMIT"
(
    cd openssl-src
    CC=$CC_WIN AR=$TRIPLE-ar RANLIB=$TRIPLE-ranlib RC=$TRIPLE-windres \
        ./Configure mingw64 no-shared no-tests --prefix="$PREFIX" --libdir=lib
    make -j"$JOBS" build_libs
    make install_dev
)

# SDL3 static, so the shipped exe has no SDL3.dll beside it
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-3.4.12.tar.gz \
    "$SDL3_SHA256" sdl3.tar.gz
tar -xzf sdl3.tar.gz
cmake -S SDL3-3.4.12 -B sdl3-build "${CMAKE_DEP[@]}" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF
cmake --build sdl3-build -j"$JOBS"
cmake --install sdl3-build

# Ubuntu's MinGW (11.0) lacks mkdtemp(), which some of the repos' tests call (MSYS2's newer
# mingw-w64 has it). A guarded shim, included into every C/C++ file through the toolchain
# file; written last so the dependency builds above are unaffected.
cat > "$PREFIX/mkdtemp-shim.h" <<'EOF'
#ifndef FN_MKDTEMP_SHIM_H
#define FN_MKDTEMP_SHIM_H
#include <direct.h>
#include <io.h>
#include <string.h>
#include <_mingw.h>
#if defined(__MINGW64_VERSION_MAJOR) && __MINGW64_VERSION_MAJOR < 12
static inline char *mkdtemp(char *tmpl)
{
    if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0 || _mkdir(tmpl) != 0)
        return 0;
    return tmpl;
}
#endif
#if defined(__MINGW64_VERSION_MAJOR) && __MINGW64_VERSION_MAJOR < 12 && !defined(TIME_UTC)
/* ... and C11 timespec_get()/TIME_UTC (the H89 desktop's boot_smoke.c uses them). Built on
   gettimeofday() so that no <windows.h> is pulled into every file: it defines `interface`. */
#include <time.h>
#include <sys/time.h>
#define TIME_UTC 1
static inline int timespec_get(struct timespec *ts, int base)
{
    struct timeval tv;
    if (base != TIME_UTC || gettimeofday(&tv, 0) != 0)
        return 0;
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = (long)tv.tv_usec * 1000;
    return base;
}
#endif
#if defined(__cplusplus) && defined(__MINGW64_VERSION_MAJOR) && __MINGW64_VERSION_MAJOR < 12
/* The msvcrt headers also lack std::c16rtomb (Mesen's UTF8Util.cpp uses it): UTF-16 to UTF-8, with
   a high surrogate held in the mbstate_t between calls. */
#include <cstddef>
#include <cwchar>
namespace std {
inline size_t c16rtomb(char *out, char16_t unit, mbstate_t *state)
{
    static mbstate_t own;
    unsigned *high = reinterpret_cast<unsigned *>(state ? state : &own);
    unsigned cp = unit;
    if (*high) {
        if (unit < 0xDC00 || unit > 0xDFFF) { *high = 0; return (size_t)-1; }
        cp = 0x10000 + ((*high - 0xD800) << 10) + (unit - 0xDC00);
        *high = 0;
    } else if (unit >= 0xD800 && unit <= 0xDBFF) {
        *high = unit;
        return 0;
    } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
        return (size_t)-1;
    }
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
}
#endif
#endif
EOF
cat >> "$PREFIX/toolchain.cmake" <<EOF
set(CMAKE_C_FLAGS_INIT "-include $PREFIX/mkdtemp-shim.h")
set(CMAKE_CXX_FLAGS_INIT "-include $PREFIX/mkdtemp-shim.h")
EOF

echo "MinGW dependencies installed in $PREFIX"
