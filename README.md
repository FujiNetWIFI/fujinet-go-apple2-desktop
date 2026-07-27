# FujiNet Go Apple II — desktop

A self-contained Apple II with FujiNet built in. One shared C session core
plus four native frontends: GTK4/libadwaita on GNOME, Qt6 Widgets on KDE,
AppKit on macOS and Win32 on Windows.

Emulation is AppleWin's libretro core. FujiNet is not a subprocess: the
firmware is built as a shared library, `dlopen`'d into the process, and joined
to the emulator over loopback TCP 1985 as a SmartPort-over-SLIP device in
slot 7 — the slot the Apple //e autostart ROM scans before the Disk ][ in
slot 6, so the machine boots straight into FujiNet's CONFIG.

This is the second member of the FujiNet Go desktop family;
[`fujinet-go-adam-desktop`](https://github.com/FujiNetWIFI/fujinet-go-adam-desktop)
is the model repository, and its `PORTING.md` is the specification this
follows.

> **Status: in progress.** The machine boots into FujiNet's CONFIG, paced to
> display vsync, with keyboard and paddle input, disk/ROM import, and a 6502
> debugger with its own native window on all four frontends. Linux, macOS,
> Windows and both flatpaks are green in CI, which also builds the Windows
> installer and the macOS bundle and can cut a release. Still to come: code
> signing for the macOS bundle. See `TODO`.

## Building

```sh
git clone https://github.com/FujiNetWIFI/fujinet-go-apple2-desktop
cd fujinet-go-apple2-desktop
cmake -B build -G Ninja
cmake --build build
./build/frontends/gnome/fujinet-go-apple2-gnome
```

No preparation step: the build provides its own dependencies. `git clone`
without `--recurse-submodules`, a source tarball with no git metadata, and a
flatpak build all end up with a usable checkout. CI deliberately does nothing
to prepare the tree, so this promise is tested on every push.

Build dependencies: CMake ≥ 3.20, Ninja, a C99/C++17 compiler, Python 3,
SDL3, zlib, and Boost headers (header-only — no compiled Boost is linked). The
GNOME frontend additionally needs libadwaita ≥ 1.4 / GTK ≥ 4.10, and the KDE
one Qt6 ≥ 6.4 Widgets. WebKitGTK 6.0 and QtWebEngine are optional; without
them the FujiNet web UI opens in the system browser.

### Useful options

| Option | Default | Meaning |
|---|---|---|
| `FRONTEND` | `all` | `gnome`, `kde`, `macos`, `windows`, `all`, `none`. `all` considers only the host's viable frontends. |
| `WITH_FUJINET` | `ON` | Build and embed the FujiNet runtime. |
| `WITH_WEBVIEW` | `ON` | Embed the FujiNet web UI rather than opening a browser. |
| `WITH_APPLE_ROMS` | `ON` | Embed the Apple II system ROMs (see `COMPLIANCE.md`). |
| `APPLEWIN_SRC` | — | Build against an out-of-tree AppleWin checkout instead of the pinned submodule. |
| `FUJINET_SRC` | — | Likewise for the FujiNet firmware. |
| `APPLEWIN_RESTAGE` | `OFF` | Re-stage AppleWin — how to pick up uncommitted edits in an `APPLEWIN_SRC` checkout. |

Developing against local checkouts:

```sh
cmake -B build -G Ninja \
      -DAPPLEWIN_SRC=$HOME/Workspace/AppleWin \
      -DFUJINET_SRC=$HOME/Workspace/fujinet-pc-apple2 \
      -DAPPLEWIN_RESTAGE=ON
```

## Apple II system ROMs

The Apple II ROMs are Apple copyrighted firmware and are **not** freely
licensed.

- **`WITH_APPLE_ROMS=ON`** (the default) compiles them in, which is what makes
  the machine boot out of the box. AppleWin itself has shipped this way for
  over a decade of continuous public use, and this project follows that
  precedent: CI builds and every published release use this configuration.
- **`WITH_APPLE_ROMS=OFF`** builds without them. Supply your own with
  **Import System ROMs…**, which copies them into
  `~/.local/share/fujinet-go-apple2/roms` (override with `APPLE2_ROM_DIR`).
  AppleWin looks ROMs up by exact filename, so keep the names
  (`Apple2e_Enhanced.rom`, `DISK2.rom`, …). FujiNet still works with no ROMs
  at all — its SmartPort card firmware is not Apple's and stays embedded.
  This is for anyone who cannot embed Apple's firmware under their own
  packaging policy (a Linux distribution, most notably); CI keeps it built
  and tested via the `linux` job's `build-noroms` step even though nothing
  published from this repository uses it.

See `COMPLIANCE.md` for the reasoning and how the `OFF` build is enforced.

## Keyboard

Everything goes to the Apple II except **F10** (menu), **F11** (fullscreen)
and **F12** (debugger). In particular **Alt is not passed to the window
manager** — the Alt keys are Open Apple and Closed Apple.

On macOS the Apple keys are the two **Option** keys, not Command: Command is
how every menu shortcut on the system is typed, and swallowing it would cost
you ⌘Q and ⌘W. Full screen is ⌃⌘F.

In the debugger: **F5** pause/continue, **F7** step into, **F8** step over,
**Shift+F8** step out; click a disassembly line to toggle a breakpoint. The
video pane decodes a text, lo-res or hi-res **page** — which is not the same
as what the machine is displaying, and is the point: it shows the buffer a
program is drawing into before it flips to it.

## Diagnostics

| Variable | Effect |
|---|---|
| `APPLE2_PACE_LOG=1` | once-per-second frame pacing: fps, frames behind, frames on vsync |
| `APPLE2_ROM_DIR` | where a `WITH_APPLE_ROMS=OFF` build reads system ROMs |
| `APPLE2_OPEN_DEBUGGER=1` | open the debugger window at launch |
| `APPLE2_DEBUGGER_VIEW=N` | open the debugger on video page N (0 = text page 1, 4 = hi-res page 1) |
| `FUJINET_LIB` | explicit path to `libfujinet.so`/`.dylib`/`.dll` |
| `FUJINET_WEBUI_BIND` | override the web admin bind address (default `127.0.0.1`) |

## Packaging and releases

| Platform | Artifact | Built by |
|---|---|---|
| Linux | `.flatpak` bundle, one per frontend | `build-aux/flatpak/online.fujinet.go.apple2.{gnome,kde}.yml` |
| macOS | `.app` bundle, zipped with `ditto` | the `macos` CI job |
| Windows | portable folder **and** an NSIS installer (per-user, no UAC) | the `windows` CI job + `build-aux/windows/installer.nsi` |

The macOS bundle is **unsigned**: Gatekeeper blocks it on first launch until
it is opened from the right-click menu. Signing and notarisation need an Apple
Developer identity in the repository secrets.

The Windows frontend has no native host here, so it is developed by
cross-compiling with `cmake/toolchains/mingw-w64.cmake` and smoke-testing
under wine — the header of that file is the full recipe, including the
zlib and Boost shims the cross sysroot lacks. CI then builds it natively
under MSYS2/UCRT64 and checks the **import table** of both the `.exe` and
`fujinet.dll` against a system-DLL whitelist (`objdump -p`, not `ldd`, which
resolves against the build machine's `PATH` and will happily call a missing
dependency satisfied).

### Cutting a release

The version lives in `project(... VERSION)` in `CMakeLists.txt` and nowhere
else. Bump it, add a matching `<release version="…">` to every
`frontends/*/data/*.metainfo.xml`, then push a `v<version>` tag. The release
job refuses to publish if the tag, `CMakeLists.txt` and the metainfo disagree,
and creates the release as a **draft** so the notes can be written before it
goes out.

## Layout

```
cmake/        dependency provisioning, AppleWin staging, FujiNet runtime
core/
  include/    apple2session.h -- THE frontend contract
  applewin/   the libretro host; the only TU that includes AppleWin headers
  src/        session, pacing, settings, paths, input, audio, gamepad, FujiNet
  tests/      ctest suite
frontends/    one directory per native toolkit
tools/        AppleWin staging patches, FujiNet build recipe, icons, debugger font
third_party/  pinned submodules
```

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.
