#!/usr/bin/env python3
# Validates the relocation tables inside a .3dsx file.
#
# 3dsxtool builds each segment's relocation table by walking a per-word
# bitmap of "needs patching" words and chaining it into (skip, patch)
# pairs, each capped at 0xFFFF words, against the *page-aligned* word
# range of that segment (how the loader actually maps it in memory) --
# not the raw segment size written to the file. 3dsxtool never re-checks
# that the pairs it emits sum back up to that range. If a segment's table
# overshoots by even one word -- an unhandled relocation type, a linker
# quirk, an off-by-one -- the loader applies patches past the end of the
# segment, corrupting whatever memory comes next. 3dsxtool exits 0, the
# file passes every static check, and it simply fails to boot with no
# diagnostic from anything in the chain. This re-derives the expected
# word count per segment from the header and confirms the relocation
# table never accounts for more than that.
import struct
import sys

PAGE = 0x1000


def align(n, a=PAGE):
    return (n + a - 1) & ~(a - 1)


def die(msg):
    print(f"check3dsx: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        die(f"usage: {sys.argv[0]} <file.3dsx>")

    path = sys.argv[1]
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        die(f"cannot read {path}: {e}")

    if len(data) < 32:
        die("file too small to contain a 3DSX header")

    try:
        magic, header_size, reloc_hdr_size, fmt_ver, flags, \
            code_size, rodata_size, data_size, bss_size = \
            struct.unpack_from("<4sHHIIIIII", data, 0)
    except struct.error as e:
        die(f"malformed header: {e}")

    if magic != b"3DSX":
        die(f"bad magic {magic!r}, not a 3DSX file")
    if header_size < 32:
        die(f"header size {header_size} is smaller than the minimum 32 bytes")
    if reloc_hdr_size == 0 or reloc_hdr_size % 4 != 0:
        die(f"relocation header size {reloc_hdr_size} is not a positive multiple of 4")

    for name, size in (("code", code_size), ("rodata", rodata_size), ("data", data_size), ("bss", bss_size)):
        if size % 4 != 0:
            die(f"{name} segment size {size} is not word-aligned")
    if bss_size > data_size:
        die(f"bss size {bss_size} exceeds data segment size {data_size}")

    n_kinds = reloc_hdr_size // 4  # relocation "kinds" per segment (absolute, relative, ...)

    off = header_size  # relocation headers start right after the (possibly extended) header
    if off + 3 * reloc_hdr_size > len(data):
        die("file truncated before relocation headers")

    seg_sizes = (code_size, rodata_size, data_size)
    seg_names = ("code", "rodata", "data")

    reloc_counts = []
    for _ in range(3):
        reloc_counts.append(struct.unpack_from(f"<{n_kinds}I", data, off))
        off += reloc_hdr_size

    # Segments follow the relocation headers, back-to-back, at their raw
    # (non-page-aligned) file sizes -- the data segment's BSS tail isn't stored.
    off += code_size
    off += rodata_size
    off += data_size - bss_size
    if off > len(data):
        die("file truncated before end of segment data")

    # Each (segment, kind) pair -- e.g. code/absolute, code/relative -- is its
    # own independent walk over the segment, starting back at word 0. They are
    # not chained together, so each is checked against the bound separately.
    for seg in range(3):
        expected_words = align(seg_sizes[seg]) // 4
        for kind in range(n_kinds):
            accounted = 0
            for _ in range(reloc_counts[seg][kind]):
                if off + 4 > len(data):
                    die(f"{seg_names[seg]} relocation table truncated")
                skip, patch = struct.unpack_from("<HH", data, off)
                off += 4
                accounted += skip + patch
                if accounted > expected_words:
                    die(
                        f"{seg_names[seg]} segment relocation table (kind {kind}) overshoots its "
                        f"{expected_words}-word bound (accounted {accounted} words so far) -- "
                        f"unencodable/corrupt relocation, will misbehave on every loader"
                    )

    print(f"check3dsx: OK ({path}, {len(data)} bytes)")


if __name__ == "__main__":
    main()
