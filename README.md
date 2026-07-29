# 3DS-CLI [![Release](https://github.com/cmdada/3DS-CLI/actions/workflows/release.yml/badge.svg)](https://github.com/cmdada/3DS-CLI/actions/workflows/release.yml)

Run a real RISC-V Linux environment inside the Nintendo 3DS homebrew launcher.

3DS-CLI embeds [mini-rv32ima-mmu](https://github.com/cmdada/mini-rv32ima-mmu),
a fork of cnlohr's compact mini-rv32ima RISC-V emulator extended with a full
Sv32 MMU, S-mode, and a built-in SBI, into a 3DS homebrew app. It boots a
stock RV32 Linux kernel (6.6, glibc userspace, real virtual memory - no NOMMU
hacks) from the SD card while still running inside the normal 3DS Horizon OS.

![3DS-CLI running on a Nintendo 3DS](example.jpg)

## What it does

- Boots a RISC-V Linux image on 3DS hardware through software emulation.
- Displays a custom ANSI/xterm terminal emulator on the top screen with full 16/256/24-bit colour support, zoom, and viewport panning.
- Provides a custom bottom-screen touch keyboard for typing commands.
- Persists real changes to a real filesystem: the root filesystem is a
  virtio-blk device backed directly by `rootfs.ext2` on the SD card, so
  writes go straight to disk as they happen — nothing is lost if the app
  crashes or the console loses power, and there's no RAM snapshot to save.
- Gives the guest 64MB of swap on a second virtio-blk device, so it isn't
  capped at the modest amount of RAM the app can claim from the 3DS heap
  (see [Swap](#swap) below).
- Gets you online: a virtio-net device NATs the guest out through the 3DS's
  real WiFi connection (DHCP, TCP, UDP, DNS all work; see
  [Networking](#networking) below for exact scope).
- Feeds the guest kernel real hardware entropy (virtio-rng, from the 3DS's
  hardware RNG) and the real date/time (a goldfish-rtc device backed by the
  3DS's hardware clock) instead of emulator defaults.
- Hands the guest the console's actual hardware: the real SD card and (with
  extended homebrew permissions) the NAND filesystems, plus the battery,
  motion sensors, sliders, cameras, microphone and speakers — see
  [3DS hardware passthrough](#3ds-hardware-passthrough) below.
- Includes a prebuilt image with bash, htop, neofetch, nano, vim, tree, wget,
  dropbear (ssh), and the usual BusyBox/util-linux tools.
- Can be used as a starting point for testing custom RISC-V Linux images, kernels,
  and Buildroot experiments on a 3DS — the `buildroot/` directory in this repo
  builds the kernel `Image` and `rootfs.ext2` from scratch.

## Installation

### Option 1: Universal Updater
Install directly on your 3DS via [Universal Updater](https://db.universal-team.net/3ds/3ds-cli): find 3DS-CLI in the app and it will handle downloading and placing the files for you.

### Option 2: Manual
1. Download the zip from the latest GitHub release.
2. Copy `3ds-cli.3dsx` to `sdmc:/3ds/` and `Image` to the root of your SD card.
   That's the whole install — the root filesystem is bundled inside `Image`,
   and the app unpacks it to `sdmc:/rootfs.ext2` on first boot.
3. Launch 3DS-CLI from the Homebrew Launcher and wait for Linux to boot
   (the first boot takes longer because of the rootfs unpack).
4. Log in as `root` with a blank password.
5. Anything you write to disk (files, package installs, config changes) persists
   directly into `rootfs.ext2` — no separate save step needed. Delete that file
   if you ever want a factory reset; it gets recreated on the next boot.

## Networking

If the 3DS is connected to WiFi, the guest gets a DHCP lease on a private
`10.0.2.0/24` subnet (gateway `10.0.2.2`) and outbound TCP/UDP/DNS traffic is
NAT'd out through the console's real connection — `wget`, `dropbear`/`ssh`,
`ntpd`, etc. all work. Two caveats, both inherent to running as unprivileged
3DS homebrew (no raw sockets):

- `ping`/ICMP only gets a reply from the gateway (`10.0.2.2`) itself, as a
  reachability check of the emulator's NAT layer — it can't reach real
  internet hosts.
- No inbound connections: the 3DS isn't reachable from the network, only the
  reverse.

## Swap

Guest RAM is whatever the app can get from the 3DS heap — 64MB at best, and
less on Old 3DS models, where the allocator settles for whatever it can find.
To keep that from being a hard ceiling, the app exposes a second virtio-blk
device (`/dev/vdb`) backed by a 64MB `sdmc:/swap.img`, which the guest turns
into swap at boot.

`swap.img` is created at launch and removed when you exit, so it isn't a file
you have to manage or clean up — nothing in it is worth keeping across boots.
If it can't be created (full or read-only SD card), the guest falls back to a
32MB zram device instead, and you'll see `Swap: unavailable` in the boot log.

Disk swap is the default rather than zram because the swap I/O runs natively
on the 3DS (plain `fread`/`fwrite` on the SD card), whereas zram would have to
compress every page on the *emulated* RISC-V CPU and would take its space out
of the same small RAM pool. The honest caveat: virtio-blk I/O is synchronous
on the emulation thread, so a workload that swaps heavily will visibly stall
the guest. Swap keeps things alive that would otherwise be OOM-killed; it
doesn't make them fast.

## 3DS hardware passthrough

The guest gets the console itself, not just a virtual machine, under
`/mnt/3ds`. It's a single 9P mount served by the app (`source/virtio_9p.h`),
so every access is a real Horizon call on the 3DS side — the console and the
guest never disagree about what's on the card.

```
/mnt/3ds/sd     the real SD card, read-write
/mnt/3ds/nand   CTR NAND filesystem, read-only
/mnt/3ds/twl    TWL NAND filesystem, read-only
/mnt/3ds/hw     sensors, power, camera, mic, speakers
```

`sd` is the same filesystem the Homebrew Launcher reads, which makes it the
easy way to get files in and out: drop something on the SD card from your PC
and it's there in the guest, and vice versa.

`nand` and `twl` need permissions plain homebrew doesn't get. Under Luma3DS
with extended homebrew permissions they appear; otherwise they're simply
absent from the listing. They're **read-only, enforced by the app** rather
than by a guest mount flag — Horizon has its own view of those filesystems
and nothing on the 3DS side would keep it consistent with guest writes.

### The `hw` tree

Plain files, so anything can read them — no driver, no library, no ioctls:

```sh
cat /mnt/3ds/hw/battery        # 87
cat /mnt/3ds/hw/charging       # 1
cat /mnt/3ds/hw/accel          # 12 -4 251     (x y z)
cat /mnt/3ds/hw/gyro           # 0 1 -2        (roll pitch yaw)
cat /mnt/3ds/hw/slider_3d      # 0.42
cat /mnt/3ds/hw/model          # New 3DS XL
cat /mnt/3ds/hw/info           # short summary of the above
```

Also `battery_voltage`, `adapter`, `shell`, `steps`, `wifi`,
`slider_volume`, `region`, `language`, `firmware`.

The notification LED and the WiFi/power LEDs are writable:

```sh
echo "info 255 0 0" > /mnt/3ds/hw/leds   # notification LED red (R G B)
echo "wifi 1"       > /mnt/3ds/hw/leds
echo "power 2"      > /mnt/3ds/hw/leds
```

Camera, microphone and speakers are files too:

```sh
cat /mnt/3ds/hw/camera_outer.rgb565 > shot.raw   # one 400x240 RGB565 frame
cat /mnt/3ds/hw/mic.pcm  > rec.pcm               # signed 16-bit mono, 16kHz
cat music.pcm > /mnt/3ds/hw/audio.pcm            # signed 16-bit stereo, 32730Hz
```

A camera frame is captured per `open()`, so re-open to get a newer one. The
capture takes up to a second, during which the guest is stopped — the 9P
server runs on the emulation thread, so a slow hardware call blocks it.
Reading the mic or writing audio doesn't have that problem; both stream and
never block, dropping samples instead of stalling the guest.

Sensor readings are also available the conventional way, as a normal evdev
device (`/dev/input/event0`) carrying the accelerometer, gyroscope and both
sliders as absolute axes — for programs that would rather read `input_event`
structs than parse text.

### Caveats

Speaker output needs the console's DSP firmware, which the app loads at
first use. Where that isn't available, writes to `audio.pcm` fail with
`EIO` rather than silently going nowhere. This is the one part of the tree
that has not been confirmed working end-to-end — Citra has no DSP firmware
to load, so it could only be verified as far as "fails cleanly".

`mic.pcm` is a continuous stream: it returns whatever has been captured
since your last read, padded with silence when the microphone hasn't
produced anything yet. It therefore never reaches EOF, so read it with
`dd count=…` rather than plain `cat`.

`df` reports free space from Horizon; when that query isn't answered (Citra's
virtual SD card doesn't) it falls back to a nominal 1GB, because reporting
zero makes anything that checks for space refuse to write.

## Controls

| Button | Action |
|---|---|
| **L** or **Y** | Zoom out |
| **R** or **X** | Zoom in |
| **ZL** | Toggle auto-follow cursor on/off |
| **ZR** | Toggle font (8x8 ↔ 5x7 compact) |
| **Circle Pad** | Pan viewport (also disables auto-follow) |
| **D-Pad** | Send arrow keys to Linux |
| **START** | Quit and return to Homebrew Launcher (disk writes are already persisted live) |

| Key | Action |
|---|---|
| **SHF** | Toggle shift (uppercase) |
| **?#1** | Switch to symbol layer 1 |
| **#+=** | Switch to symbol layer 2 |
| **ABC** | Return to alphabetic layer |
| **CTL** | Ctrl modifier — tap CTL, then tap a letter for Ctrl+key |
| **TAB** | Send tab |
| **ESC** | Send escape |
| **ENT** | Send enter |
| **DEL** | Send backspace |

## Building

Install devkitPro with devkitARM and libctru, then run:

```sh
make        # builds 3ds-cli.3dsx (and validates it, see below)
make cia    # also builds 3ds-cli.cia (needs bannertool + makerom)
```

`make` finishes by running `tools/check3dsx.py` over the result. A .3dsx can
link cleanly, pass `3dsxtool`, and still be rejected outright by every loader
over a single unencodable relocation — with no warning from any of them. The
check turns that into a build failure. See the comment at the top of that
script if it ever fires.

If you change `source/3ds-cli.dts` (e.g. to add a new emulated device), regenerate
the embedded device tree blob with `make dtb` (needs `dtc` and `python3`).

### Building the guest kernel and rootfs

`buildroot/` is a self-contained Buildroot external tree that produces `Image`
(the RISC-V kernel) and `rootfs.ext2` (the disk image the app boots from):

```sh
# Fetch upstream Buildroot once (or point at your own checkout):
wget https://buildroot.org/downloads/buildroot-2024.02.9.tar.gz
tar xf buildroot-2024.02.9.tar.gz -C buildroot-native

cd buildroot-native/buildroot-2024.02.9
make BR2_EXTERNAL=../../buildroot O=../../buildroot-native-output 3ds_defconfig
make O=../../buildroot-native-output
```

Kernel config lives in `board/3ds-cli/linux.config`; userspace package
selection is in `configs/3ds_defconfig`. Outputs land in
`buildroot-native-output/images/`.

To bundle the two into the single release-style `Image` (kernel + gzipped
rootfs, unpacked to the SD card by the app on first boot):

```sh
python3 tools/mkimage.py Image rootfs.ext2 Image-combined
```

A plain kernel `Image` with a separate `rootfs.ext2` on the SD card keeps
working too, which is handier while iterating on the rootfs.

## Star History

<a href="https://www.star-history.com/?repos=cmdada%2F3ds-cli&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=cmdada/3ds-cli&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=cmdada/3ds-cli&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=cmdada/3ds-cli&type=date&legend=top-left" />
 </picture>
</a>

## Credits

- Built with [devkitARM / libctru](https://github.com/devkitPro/libctru)
- Powered by [mini-rv32ima-mmu](https://github.com/cmdada/mini-rv32ima-mmu),
  a fork of [mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) by cnlohr

## License

GPL-3.0. See [LICENSE](LICENSE).
