# FujiNet Go Apple II — desktop

A self-contained Apple II with FujiNet built in. One shared C session core
plus native frontends: GTK4/libadwaita on GNOME, and (to come) Qt6 Widgets on
KDE, AppKit on macOS, Win32 on Windows.

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
> display vsync, with keyboard and paddle input and disk/ROM import. Still to
> come: the KDE, macOS and Windows frontends, the 6502 debugger, CI and
> packaging. See `TODO`.

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
`xxd` (AppleWin embeds its resources with it — it ships with vim), SDL3,
zlib, and Boost headers (header-only — no compiled Boost is linked). The
GNOME frontend additionally needs libadwaita ≥ 1.4 / GTK ≥ 4.10, and the KDE
one Qt6 ≥ 6.4 Widgets. WebKitGTK 6.0 and QtWebEngine are optional; without
them the FujiNet web UI opens in the system browser.

### Useful options

| Option | Default | Meaning |
|---|---|---|
| `FRONTEND` | `all` | `gnome`, `kde`, `all`, `none`. `all` considers only the host's viable frontends. |
| `WITH_FUJINET` | `ON` | Build and embed the FujiNet runtime. |
| `WITH_WEBVIEW` | `ON` | Embed the FujiNet web UI rather than opening a browser. |
| `WITH_APPLE_ROMS` | `ON` | Embed the Apple II system ROMs. **See `COMPLIANCE.md` before distributing.** |
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
  the machine boot out of the box — but an artifact built that way **must not
  be redistributed.**
- **`WITH_APPLE_ROMS=OFF`** builds without them. Supply your own with
  **Import System ROMs…**, which copies them into
  `~/.local/share/fujinet-go-apple2/roms` (override with `APPLE2_ROM_DIR`).
  AppleWin looks ROMs up by exact filename, so keep the names
  (`Apple2e_Enhanced.rom`, `DISK2.rom`, …). FujiNet still works with no ROMs
  at all — its SmartPort card firmware is not Apple's and stays embedded.

Read `COMPLIANCE.md` before publishing anything.

## Keyboard

Everything goes to the Apple II except **F10** (menu) and **F11**
(fullscreen). In particular **Alt is not passed to the window manager** — the
Alt keys are Open Apple and Closed Apple. On a Mac keyboard the Command keys
work too.

## Diagnostics

| Variable | Effect |
|---|---|
| `APPLE2_PACE_LOG=1` | once-per-second frame pacing: fps, frames behind, frames on vsync |
| `APPLE2_ROM_DIR` | where a `WITH_APPLE_ROMS=OFF` build reads system ROMs |
| `FUJINET_LIB` | explicit path to `libfujinet.so`/`.dylib`/`.dll` |
| `FUJINET_WEBUI_BIND` | override the web admin bind address (default `127.0.0.1`) |

## Layout

```
cmake/        dependency provisioning, AppleWin staging, FujiNet runtime
core/
  include/    apple2session.h -- THE frontend contract
  applewin/   the libretro host; the only TU that includes AppleWin headers
  src/        session, pacing, settings, paths, input, audio, gamepad, FujiNet
  tests/      ctest suite
frontends/    one directory per native toolkit
tools/        AppleWin staging patches, FujiNet build recipe, icons
third_party/  pinned submodules
```

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.
