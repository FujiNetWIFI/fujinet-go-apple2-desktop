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
  what any public download must be.

### The packaging path inverts the default

A CMake default is a convenience for whoever is at the keyboard; it is not a
distribution policy, and "remember to pass `-DWITH_APPLE_ROMS=OFF` when you
cut a release" is exactly the kind of rule that gets forgotten once. So every
path that produces something for other people pins the option to `OFF` in the
file that defines the artifact, not in the command that invokes it:

| Path | Where `OFF` is set |
|---|---|
| GNOME flatpak | `build-aux/flatpak/online.fujinet.go.apple2.gnome.yml` |
| KDE flatpak | `build-aux/flatpak/online.fujinet.go.apple2.kde.yml` |
| Windows folder + installer | the `windows` job in `.github/workflows/ci.yml` |

Those are the only artifacts CI uploads, and the release job attaches nothing
else — so **nothing published from this repository contains Apple firmware**,
and making that untrue takes a deliberate edit to a tracked file rather than
an omission. The `linux` job still builds and tests `ON` so the embedded path
cannot rot.

The cost is that a local `flatpak-builder` run also yields a ROM-less bundle.
That is the right way round: the failure mode is "the machine asks for ROMs",
not "you have quietly shipped Apple's firmware to strangers".

### How `WITH_APPLE_ROMS=OFF` works

AppleWin's `common2` links the `apple2roms` target unconditionally, so it has
to exist either way — what changes is whether it carries Apple's bytes. With
the option off, `tools/applewin/generate-rom-loader.py` generates a
replacement that defines the same `apple2roms::data` map but fills it by
reading ROM files from the user's ROM directory
(`$XDG_DATA_HOME/fujinet-go-apple2/roms`, overridable with `APPLE2_ROM_DIR`)
on first use. The id → filename table is parsed out of AppleWin's own
`resource/CMakeLists.txt` so it cannot drift from the staged tree.

The split is by kind: every firmware image (`.rom`, `.ROM`, `.bin`) is
externalised, and AppleWin's own artwork and fonts stay compiled in. That is
deliberately conservative — it covers Apple's monitor/Applesoft/Disk ][/SSC
firmware and also the third-party card ROMs (Mockingboard, Mouse,
ThunderClock, …) whose redistribution terms have not been established either.

The one firmware image kept embedded is **`spoverslip.bin`**, the
SmartPort-over-SLIP card from the FujiNetWIFI AppleWin fork — it is not
Apple's, and without it there is no FujiNet at all.

Users supply the ROMs through **Import System ROMs…**, which copies them into
the ROM directory under their own names (AppleWin looks them up by exact
filename) and restarts the session. Without any ROMs the app reports what is
missing and where to put it, rather than showing a black screen.

**This is enforced, not just documented.** The `no_embedded_roms` ctest takes a
distinctive slice of every firmware image in the staged resource directory and
greps the built binary for it; a ROM that gets compiled in despite the option
fails the build. It runs only in the configuration it describes.

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

Applied at staging time by `tools/applewin/patch-staged-tree.py`. These are
genuine fixes and gaps, not local preferences, and should be sent to
`FujiNetWIFI/AppleWin` rather than carried indefinitely:

- `SmartPortOverSlip.cpp` — **the libretro frontend never starts the SLIP
  listener at all.** Upstream starts it from `LoadConfiguration()` and from
  the Windows property sheet, and the libretro path reaches neither; the
  card's constructor only logs. FujiNet dials forever and nothing answers.
  Starting it from the card constructor is right regardless of frontend:
  inserting the card is when its listener should come up.
- `devrelay/service/Listener.h` — the listener binds `0.0.0.0` by default,
  putting an unauthenticated block-device channel on the network. Bound to
  loopback instead; it has to be the *default* that changes, because
  `LoadConfiguration()` re-initialises from it.
- `devrelay/service/Listener.cpp` — `start()`/`stop()` mishandle the listener
  thread across emulator re-inits, calling `std::terminate` on the second
  machine-type change.
- `frontends/libretro/rdirectsound.cpp` — a sample-doubling bug on every
  ring-buffer wrap (an audible ~2.7 Hz "gallop").
- `frontends/libretro/retroregistry.cpp` — exposing the SmartPort-over-SLIP
  card in the slot 5 and slot 7 core options, and adding a `slot6` option at
  all (upstream has none, and `CardManager`'s constructor hard-inserts a
  Disk ][ there).

The remainder are build-integration edits (building as a subdirectory,
resolving zlib/Boost our way, skipping libslirp/libpcap) with no behavioural
effect.
