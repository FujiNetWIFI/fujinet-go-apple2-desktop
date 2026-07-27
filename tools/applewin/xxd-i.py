#!/usr/bin/env python3
"""Reproduce `xxd -i <file>` without needing xxd.

AppleWin's resource/CMakeResources.cmake embeds every ROM and image by
shelling out to `xxd -i`, which is part of vim and is therefore absent more
often than not:

  * the GNOME and KDE flatpak SDKs do not ship it, and a flatpak build has no
    network to go and fetch it;
  * MSYS2 does not install it by default;
  * the shell redirection the original command uses (`xxd -i in > out`) also
    needs a shell, which is why it failed under a native Windows cmake even
    once xxd was present.

Python is already a hard build dependency (the staging patcher runs on it),
and every one of those environments has it. Writing the output file directly
sidesteps the redirection problem as well.

The output has to match xxd byte for byte in the ways that matter: the symbol
name is the filename with [ ./-] replaced by underscores -- which is what
AppleWin's add_resources() assumes when it declares `extern` for it -- and the
length variable is `unsigned int`.

usage: xxd-i.py <input-binary> <output-cpp>
"""

import re
import sys
from pathlib import Path

PER_LINE = 12


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: xxd-i.py <input-binary> <output-cpp>")

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    data = src.read_bytes()

    # Same transformation xxd applies to derive the C identifier.
    symbol = re.sub(r"[ ./-]", "_", src.name)

    out = [f"unsigned char {symbol}[] = {{"]
    for i in range(0, len(data), PER_LINE):
        chunk = data[i:i + PER_LINE]
        line = ", ".join(f"0x{b:02x}" for b in chunk)
        out.append(f"  {line}," if i + PER_LINE < len(data) else f"  {line}")
    out.append("};")
    out.append(f"unsigned int {symbol}_len = {len(data)};")
    out.append("")

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(out), encoding="ascii")


if __name__ == "__main__":
    main()
