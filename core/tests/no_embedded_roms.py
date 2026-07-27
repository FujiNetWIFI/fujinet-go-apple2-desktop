#!/usr/bin/env python3
"""Assert that a WITH_APPLE_ROMS=OFF binary really carries no Apple ROMs.

This is the test that makes the COMPLIANCE.md claim checkable rather than
aspirational: it takes a distinctive slice out of each firmware image in
AppleWin's resource directory and greps the built binary for it. Anything
found is a ROM that got compiled in despite the option, which would make the
artifact non-redistributable.

spoverslip.bin is expected to be present -- it is the SmartPort-over-SLIP card
firmware from the FujiNetWIFI AppleWin fork, not Apple's, and FujiNet does not
work without it.

usage: no_embedded_roms.py <binary> <applewin-resource-dir>
"""

import sys
from pathlib import Path

EXPECTED_PRESENT = {"spoverslip.bin"}
PROBE = 64


def probe_bytes(data):
    """A slice distinctive enough that finding it means something.

    The first version of this only rejected chunks that were mostly ZERO,
    which let a run of 0xFF through -- and TK3000e.rom starts with 16KB of
    0xFF padding. A 64-byte run of 0xFF occurs in almost any binary of a
    reasonable size, so the test reported that ROM as embedded in artifacts
    that did not contain it. It went unnoticed because the small test binary
    it runs against happens to have no such run, while the shipped frontends
    do.

    So: reject any chunk dominated by a single byte value, require real
    variety, and scan the whole file rather than three fixed offsets.
    """
    max_run = PROBE // 2      # no single byte value may cover half the slice
    min_distinct = 12

    for start in range(0, max(1, len(data) - PROBE), PROBE):
        chunk = data[start:start + PROBE]
        if len(chunk) != PROBE:
            continue
        if max(chunk.count(b) for b in set(chunk)) > max_run:
            continue
        if len(set(chunk)) < min_distinct:
            continue
        return chunk
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).read_bytes()
    resdir = Path(sys.argv[2])

    checked = 0
    leaked = []
    missing_expected = []
    unprobeable = []

    for f in sorted(resdir.iterdir()):
        if not f.is_file() or f.suffix.lower() not in (".rom", ".bin"):
            continue
        data = f.read_bytes()
        if len(data) < 128:
            continue
        chunk = probe_bytes(data)
        if chunk is None:
            # Nothing distinctive enough to search for -- report it rather
            # than quietly treating it as checked.
            unprobeable.append(f.name)
            continue
        checked += 1
        present = chunk in binary
        if f.name in EXPECTED_PRESENT:
            if not present:
                missing_expected.append(f.name)
        elif present:
            leaked.append(f.name)

    if checked == 0:
        sys.exit("no_embedded_roms: no firmware images found in "
                 f"{resdir} -- the test would pass vacuously")

    for name in leaked:
        print(f"FAIL: {name} is embedded in a WITH_APPLE_ROMS=OFF build")
    for name in missing_expected:
        print(f"FAIL: {name} should be embedded but is not")

    if leaked or missing_expected:
        return 1
    if unprobeable:
        print("no_embedded_roms: NOT checked (no distinctive slice): "
              + ", ".join(sorted(unprobeable)))
    print(f"no_embedded_roms: {checked} firmware images checked; "
          f"only {', '.join(sorted(EXPECTED_PRESENT))} embedded, as intended")
    return 0


if __name__ == "__main__":
    sys.exit(main())
