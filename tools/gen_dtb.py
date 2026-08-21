#!/usr/bin/env python3
"""Regenerate source/core/default64mbdtc.h from source/3ds-cli.dts.

The header is generated, never hand-edited. Every console's build consumes it,
and CI regenerates it so a .dts change cannot ship against a stale header.

The 0xdeadc0de sentinel in the memory node marks the word main() patches at
runtime with the guest's actual usable RAM size; its offset is emitted
alongside the bytes.
"""

import subprocess
import sys
from pathlib import Path

DTS = Path("source/3ds-cli.dts")
OUT = Path("source/core/default64mbdtc.h")
SENTINEL = bytes([0xDE, 0xAD, 0xC0, 0xDE])


def main() -> int:
    dtb = Path("/tmp/3ds-cli.dtb")
    r = subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb), str(DTS)],
                       capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode())
        return 1

    data = dtb.read_bytes()
    offset = data.find(SENTINEL)
    if offset == -1:
        sys.stderr.write("sentinel 0xdeadc0de not found in DTB\n")
        return 1

    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16])
            for i in range(0, len(data), 16)]
    OUT.write_text("static const unsigned char default64mbdtb[] = {\n"
                   + ",\n".join(rows)
                   + f"\n}};\n#define DTB_MEM_SIZE_OFFSET {offset}\n")
    print(f"DTB: {len(data)} bytes, patch offset {offset:#x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
