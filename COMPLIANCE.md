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

## Apple II system ROMs

The Apple II monitor, Applesoft, and Disk ][ ROMs are Apple copyrighted
firmware and are not freely licensed. AppleWin's `apple2roms` resource
target compiles them into the binary (`xxd -i`), which is what lets the
machine boot out of the box.

This is controlled by the `WITH_APPLE_ROMS` CMake option:

- **`ON` (default)** — the ROMs are embedded, as the Android app does and as
  upstream AppleWin itself has done throughout over a decade of continuous,
  widely-used public distribution. The machine boots immediately. **CI
  builds and publishes every release artifact this way** — see below.
- **`OFF`** — build without the ROMs; the user supplies their own. Kept
  built and tested (the `linux` job's dedicated `build-noroms` step) for
  anyone who cannot embed Apple's firmware under their own policy — most
  notably a Linux distribution, which typically may not package proprietary
  firmware alongside free software regardless of how tolerated the practice
  is upstream.

### Why the shipped default embeds the ROMs

This project follows AppleWin's own long-standing precedent rather than
adopting a stricter policy of its own: AppleWin has shipped with the Apple
II ROMs compiled in by default for its entire public history, in constant
use, without that practice being challenged. Recognising that, CI builds
and publishes with `WITH_APPLE_ROMS=ON` on every platform:

- GNOME and KDE flatpaks (`build-aux/flatpak/online.fujinet.go.apple2.*.yml`)
- the macOS `.app` bundle and Windows folder/installer (the `macos` and
  `windows` jobs in `.github/workflows/ci.yml`)
- the `linux` job's primary build (`WITH_APPLE_ROMS`'s CMake default)

`OFF` is now the exception, set explicitly only where a build must not carry
Apple's firmware — currently just the `linux` job's `build-noroms` step,
which exists to keep that configuration from rotting for anyone who needs
it, not because anything published from this repository uses it.

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
ThunderClock, …) whose redistribution terms have not been established
either, so an `OFF` build stays conservative even where the shipped `ON`
default no longer is.

The one firmware image kept embedded regardless of the option is
**`spoverslip.bin`**, the SmartPort-over-SLIP card from the FujiNetWIFI
AppleWin fork — it is not Apple's, and without it there is no FujiNet at
all.

Users of an `OFF` build supply the ROMs through **Import System ROMs…**,
which copies them into the ROM directory under their own names (AppleWin
looks them up by exact filename) and restarts the session. Without any ROMs
the app reports what is missing and where to put it, rather than showing a
black screen.

**This is enforced, not just documented, for the configuration it
describes.** The `no_embedded_roms_*` ctests take a distinctive slice of
every firmware image in the staged resource directory and grep a built
binary for it; a ROM that gets compiled in despite the option fails the
build. They register only when `WITH_APPLE_ROMS=OFF`, one per binary —
`boot_smoke` plus every frontend executable built in that configuration —
so the `linux` job's `build-noroms` step keeps the claim checked even though
it no longer describes a published artifact.

Two things this got wrong at first, both worth remembering:

- It checked only the `boot_smoke` test binary. That is a poor stand-in for
  the thing whose contents actually matter, and it linked differently enough
  to hide the next problem.
- The "distinctive slice" only rejected chunks that were mostly **zero**. One
  ROM (`TK3000e.rom`) begins with 16KB of `0xFF`, and a 64-byte run of `0xFF`
  occurs in almost any sizeable binary — so the probe matched everywhere. It
  went unseen precisely because the small test binary had no such run. The
  probe now rejects any slice dominated by a single byte value, requires real
  variety, and scans the whole file; a ROM with nothing distinctive to search
  for is **reported as unchecked** rather than silently counted as passing.

Verified in both directions: the check passes on a `WITH_APPLE_ROMS=OFF`
build and fails loudly on a `WITH_APPLE_ROMS=ON` one.

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
