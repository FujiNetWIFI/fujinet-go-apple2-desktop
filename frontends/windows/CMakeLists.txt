# Windows frontend: native Win32 (GDI + DwmFlush vsync) over the shared
# session core. Built only on Windows hosts; verified by the Windows CI job
# (MSYS2/MinGW-w64) and, before that, by cross-compiling with
# cmake/toolchains/mingw-w64.cmake and running under wine.
# WIN32_EXECUTABLE => WinMain, no console window.

if(NOT WIN32)
  return()
endif()

enable_language(RC)

# The manifest carries the app version in its assembly identity, so it is
# generated from PROJECT_VERSION rather than repeating it. resource.rc picks
# the generated copy up through the RC compiler's include path (below).
configure_file(app.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" @ONLY)

add_executable(fujinet-go-apple2-windows WIN32
  main.c
  debugger/dbg_window.c
  resource.rc
)
target_link_libraries(fujinet-go-apple2-windows PRIVATE
  apple2session
  gdi32 user32 comdlg32 dwmapi shell32 comctl32
)
# The debugger window talks to the shared engine (core/include/apple2debug.h).
# CMAKE_CURRENT_BINARY_DIR is on the list for the RC compiler's sake: that is
# where the generated app.manifest lands.
target_include_directories(fujinet-go-apple2-windows PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(fujinet-go-apple2-windows PRIVATE -Wall -Wextra)

# Bundle the MinGW runtime statically so the .exe has no loose DLL deps --
# the same promise the Windows CI job checks with objdump on the import table.
target_link_options(fujinet-go-apple2-windows PRIVATE
  -static -static-libgcc -static-libstdc++)

install(TARGETS fujinet-go-apple2-windows RUNTIME DESTINATION bin)
