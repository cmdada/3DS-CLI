#!/usr/bin/env bash
# Build submission packages for the console homebrew stores.
#
#   oscwii.org      Wii, Open Shop Channel: apps/<name>/{boot.dol,meta.xml,icon.png}
#   hb-app.store    Wii U and Switch, libget: SD-relative paths + manifest.install
#
# Each package carries the guest `Image` so it installs and runs on its own.
# Build the binaries first (make wii / make wiiu / make switch), and pass the
# bundled Image produced by tools/mkimage.py.
#
# Usage: tools/package.sh <bundled-Image>
set -euo pipefail

IMAGE="${1:?usage: tools/package.sh <bundled-Image>}"
[ -f "$IMAGE" ] || { echo "package: $IMAGE not found" >&2; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist/packages"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

VERSION="${VERSION:-1.1}"
DATE="$(date +%Y%m%d)"

need() { [ -f "$1" ] || { echo "package: missing $1 (run the matching make target)" >&2; exit 1; }; }

# ── Wii: Open Shop Channel ────────────────────────────────────────────────
need "$ROOT/dist/wii/3ds-cli.dol"
APP="$WORK/osc/apps/3ds-cli"
mkdir -p "$APP"
cp "$ROOT/dist/wii/3ds-cli.dol" "$APP/boot.dol"
cp "$IMAGE" "$WORK/osc/Image"

# 128x48 is the icon size the Open Shop Channel expects. Committed rather than
# generated so this needs no image tooling, and so the icon people will
# actually see in the shop is reviewable in the repo. To regenerate:
#   magick banner.png -resize 128x48 -background '#191724' -gravity center \
#          -extent 128x48 assets/osc-icon.png
need "$ROOT/assets/osc-icon.png"
cp "$ROOT/assets/osc-icon.png" "$APP/icon.png"

cat > "$APP/meta.xml" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<app version="1">
  <name>3DS-CLI</name>
  <coder>cmdada</coder>
  <version>$VERSION</version>
  <release_date>$DATE</release_date>
  <short_description>Real RISC-V Linux on your Wii</short_description>
  <long_description>Boots a stock RV32 Linux 6.6 kernel with a glibc userspace
from your SD card, emulating a RISC-V machine with a full Sv32 MMU. The top of
the screen is an ANSI terminal, the bottom an on-screen keyboard you point at
with the Wiimote.

Comes with bash, vim, nano, htop, tree, wget and dropbear. Writes go straight
to rootfs.ext2 on the card, so nothing is lost when you quit. Networking runs
over the console's own connection.

Copy Image to the root of your SD card alongside this app. On first boot the
root filesystem is unpacked from it, which takes a few minutes; after that it
is the writable disk. Log in as root with a blank password.</long_description>
</app>
XML

(cd "$WORK/osc" && zip -qr "$OUT/oscwii-3ds-cli.zip" .)
echo "oscwii-3ds-cli.zip        $(du -h "$OUT/oscwii-3ds-cli.zip" | cut -f1)"

# ── Wii U and Switch: Homebrew App Store (libget) ─────────────────────────
# A libget package is the files at their SD-relative paths plus a manifest
# naming each one. U: means overwrite on update.
hbas() {
  local name="$1" src="$2" dest="$3"
  need "$src"
  local d="$WORK/$name"
  mkdir -p "$d/$(dirname "$dest")"
  cp "$src" "$d/$dest"
  cp "$IMAGE" "$d/Image"
  printf '[manifest]\nU: %s\nU: Image\n' "$dest" > "$d/manifest.install"
  (cd "$d" && zip -qr "$OUT/hbas-$name-3ds-cli.zip" .)
  echo "hbas-$name-3ds-cli.zip   $(du -h "$OUT/hbas-$name-3ds-cli.zip" | cut -f1)"
}

hbas wiiu   "$ROOT/dist/wiiu/3ds-cli.wuhb" "wiiu/apps/3ds-cli.wuhb"
hbas switch "$ROOT/dist/switch/3ds-cli.nro" "switch/3ds-cli.nro"

echo
echo "packages in dist/packages/"
