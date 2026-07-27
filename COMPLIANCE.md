# Licence and provenance

Per-component origin, licence, and how each enters the build. Written before
the first public build, as the family convention requires.

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | `FujiNetWIFI/fujinet-go-apple2-desktop` | GPL-3.0-or-later | the repository itself |
| AppleWin (emulator core) | `FujiNetWIFI/AppleWin`, branch `linux` | GPL-2.0-or-later | pinned submodule `third_party/applewin`, staged into `core/applewin-generated` and compiled as a subdirectory |
| FujiNet firmware | `FujiNetWIFI/fujinet-firmware` | GPL-3.0-or-later | pinned submodule `third_party/fujinet-firmware`, built as a shared library and `dlopen`'d |
| libyaml | vendored in AppleWin | MIT | compiled from the staged tree |
| minizip | vendored in AppleWin | zlib | compiled from the staged tree |
| Boost (headers only) | system | BSL-1.0 | `property_tree`, `algorithm/string`, `multi_array`; header-only, no compiled Boost library is linked |
| zlib | system | zlib | `find_package(ZLIB)` |
| SDL3 | system | Zlib | audio and gamepad only |
| mbedTLS 3.6.5 | system or built from the pinned tag | Apache-2.0 | linked into the FujiNet library |

Combining AppleWin (GPL-2.0-**or-later**) with the FujiNet firmware (GPL-3.0)
is permitted in the "or later" direction, so the combined work is effectively
**GPL-3.0**.

## Apple II system ROMs — read this before distributing a build

The Apple II monitor, Applesoft, and Disk ][ ROMs are **Apple copyrighted
firmware and are not freely licensed.** AppleWin's `apple2roms` resource
target compiles them into the binary (`xxd -i`), which is what lets the
machine boot out of the box.

This is controlled by the `WITH_APPLE_ROMS` CMake option:

- **`ON` (default)** — the ROMs are embedded, as the Android app does. The
  machine boots immediately. **An artifact built this way must not be
  publicly redistributed.** This is the development and local-use default.
- **`OFF`** — build without the ROMs; the user supplies their own. This is
  what the release job builds, and what any public download must be.

`WITH_APPLE_ROMS=OFF` is **not implemented yet** — configuring with it fails
with an explanatory error rather than silently embedding the ROMs anyway. It
needs a placeholder `apple2roms` target (AppleWin's `common2` links that
target unconditionally) plus the user ROM-import path; the two land together.
Until then, **no artifact from this repository is redistributable.**

`spoverslip.bin`, the SmartPort-over-SLIP card firmware, is part of the
FujiNetWIFI AppleWin fork and is *not* affected by the above.

## Deliberate choices

- **The 6502/65C02 disassembler is written fresh** rather than reused from
  AppleWin. Reuse would be licence-clean (GPL-2.0-or-later into GPL-3.0), but
  AppleWin's `Debugger_Disassembler.cpp` is coupled to its own console,
  `DisasmData_t` and formatting flags; a table-driven disassembler producing
  the `apple2dasm_line` shape the engine and all four frontends already
  consume is less work than untangling it. No code from AppleWin's debugger
  is present.
- **Debug symbol tables** (`bin/APPLE2E.SYM`, `A2_BASIC.SYM`,
  `A2_DOS33.SYM2`, shipped with AppleWin) are tables of *facts* — names and
  addresses — and contain no program code.
- **libslirp / libpcap are deliberately not linked.** AppleWin can emulate an
  Uthernet card through them; FujiNet does not use it, and linking them would
  break the flatpak build and the macOS/Windows self-containment checks. The
  staging step patches the detection out (see
  `tools/applewin/patch-staged-tree.py`).

## Patches carried against AppleWin

Applied at staging time by `tools/applewin/patch-staged-tree.py`. Three are
genuine upstream bug fixes and should be sent to `FujiNetWIFI/AppleWin`
rather than carried indefinitely:

- `devrelay/service/Listener.cpp` — `start()`/`stop()` mishandle the listener
  thread across emulator re-inits, calling `std::terminate` on the second
  machine-type change.
- `frontends/libretro/rdirectsound.cpp` — a sample-doubling bug on every
  ring-buffer wrap (an audible ~2.7 Hz "gallop").
- `frontends/libretro/retroregistry.cpp` — exposing the SmartPort-over-SLIP
  card in the slot 5 and slot 7 core options.

The remainder are build-integration edits (building as a subdirectory,
resolving zlib/Boost our way) with no behavioural effect.
