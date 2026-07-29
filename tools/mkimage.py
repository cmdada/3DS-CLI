#!/usr/bin/env python3
"""Bundle the kernel Image and rootfs into a single distributable file.

Layout: [kernel Image][gzipped rootfs][u32 gz_len][u32 raw_len]["3DSCLIRF"]

The app detects the 16-byte trailer at load time: the kernel part is
everything before the gzip blob, and if sdmc:/rootfs.ext2 doesn't exist yet
it inflates the blob there on first boot (one-time; the SD copy stays the
writable, persistent filesystem exactly as before). A plain kernel Image
with no trailer keeps working unchanged, so local dev flows don't need this.

Usage: mkimage.py <kernel-Image> <rootfs.ext2> <output>
"""
import gzip
import struct
import sys

MAGIC = b"3DSCLIRF"

def main():
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 1
    kernel_path, rootfs_path, out_path = sys.argv[1:4]

    with open(kernel_path, "rb") as f:
        kernel = f.read()
    if kernel[-16:-8].endswith(MAGIC) or kernel[-8:] == MAGIC:
        print("error: kernel input already has a rootfs trailer", file=sys.stderr)
        return 1

    with open(rootfs_path, "rb") as f:
        raw = f.read()
    blob = gzip.compress(raw, compresslevel=9)

    with open(out_path, "wb") as f:
        f.write(kernel)
        f.write(blob)
        f.write(struct.pack("<II", len(blob), len(raw)))
        f.write(MAGIC)

    print(f"{out_path}: kernel {len(kernel)} B + rootfs {len(raw)} B "
          f"(gz {len(blob)} B) = {len(kernel) + len(blob) + 16} B total")
    return 0

if __name__ == "__main__":
    sys.exit(main())
