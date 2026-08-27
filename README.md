# 3DS-CLI [![Release](https://github.com/cmdada/3DS-CLI/actions/workflows/release.yml/badge.svg)](https://github.com/cmdada/3DS-CLI/actions/workflows/release.yml) [![Downloads](https://img.shields.io/github/downloads/cmdada/3ds-cli/total.svg)](https://github.com/cmdada/3DS-CLI/releases)

Run real RISC-V Linux on various consoles: Nintendo's 3DS, Wii U, Switch, Wii
and GameCube, and Sony's PSP.

3DS-CLI boots a stock RV32 Linux 6.6 kernel with a glibc userspace off your SD
card, using [mini-rv32ima-mmu](https://github.com/cmdada/mini-rv32ima-mmu), mini-rv32ima with a full Sv32 MMU, S-mode and a built-in SBI,
running as ordinary homebrew.

![3DS-CLI running on a Nintendo 3DS](assets/example.jpg)

- A terminal (ANSI/xterm, 24-bit colour, zoom, panning) and a touch keyboard,
  on the two screens where the console has two, split top and bottom where it
  has one.
- Writes go straight to `rootfs.ext2` on the SD card. No save step, and nothing
  is lost if the battery dies.
- Networking over the console's WiFi. `wget`, `ssh` and `ntpd` all work.
- The guest gets real hardware: SD card, NAND, battery, sensors, sliders,
  cameras, mic, speakers.
- Comes with bash, vim, nano, htop, tree, wget, dropbear, BusyBox.
- Runs on Old 3DS and New 3DS, and on the Wii U, Switch, Wii, GameCube and
  PSP. One `Image` boots on all of them, because the guest is identical and
  only the host binary differs.

## Install

**Universal Updater** (3DS): find 3DS-CLI in
[the app](https://db.universal-team.net/3ds/3ds-cli) and it does the rest.

**Manual**, from the latest release zip. Copy `Image` to the root of the SD
card, then your console's binary. The rootfs is bundled inside `Image`, so
that's everything. First boot is slower because it unpacks `rootfs.ext2` to
the card, which is the writable disk from then on. Log in as `root` with a
blank password.

On the 3DS, Wii U, Switch and PSP you can copy just the binary and skip
`Image`: launched with none on the card, the app offers to download the latest
release's over WiFi. The Wii and GameCube have no TLS stack to do that with.

| Console | Binary | Goes to |
|---|---|---|
| 3DS | `3ds/3ds-cli.3dsx` | `sdmc:/3ds/3ds-cli/` |
| Wii U | `wiiu/3ds-cli.wuhb` | `sd:/wiiu/apps/` |
| Switch | `switch/3ds-cli.nro` | `sdmc:/switch/` |
| Wii | `wii/3ds-cli.dol` | `sd:/apps/3ds-cli/boot.dol` |
| GameCube | `gamecube/3ds-cli.dol` | an SD Gecko or SD2SP2 |
| PSP | `psp/EBOOT.PBP` | `ms0:/PSP/GAME/3ds-cli/` |

The GameCube needs an SD Gecko or SD2SP2: the rootfs is ~200MB and a memory
card cannot hold it. It also has the least RAM of the five, so it is the
tightest fit.

### What each console gives the guest

| | Guest RAM | Screens | Pointer | Network |
|---|---|---|---|---|
| 3DS | 8–64MB | two, touch below | touchscreen | yes |
| Wii U | 64MB | TV + GamePad | GamePad touch | yes |
| Switch | 512MB | one, split | touchscreen | yes |
| Wii | 48MB | one, split | Wiimote IR | yes |
| GameCube | 20MB | one, split | none, D-pad focus | no |
| PSP | 20-48MB | one, split | none, D-pad focus | yes |

## Controls

| Button | Action |
|---|---|
| **L** / **Y** | Zoom out |
| **R** / **X** | Zoom in |
| **ZL** | Toggle auto-follow cursor |
| **ZR** | Toggle font (8x8 / 5x7 compact) |
| **Circle Pad** | Pan the viewport (turns off auto-follow) |
| **D-Pad** | Arrow keys |
| **START** | Quit to the Homebrew Launcher |
| **SELECT** | Open settings menu, it's pretty self explanatory |

| Key | Action |
|---|---|
| **SHF** | Shift (uppercase) |
| **?#1** / **#+=** | Symbol layers |
| **ABC** | Back to letters |
| **CTL** | Ctrl modifier. Tap CTL, then a letter |
| **TAB** / **ESC** / **ENT** / **DEL** | Tab, escape, enter, backspace |

## Hardware passthrough

The console shows up under `/mnt/3ds`:

```
sd     the real SD card, read-write
nand   CTR NAND, read-only
twl    TWL NAND, read-only
hw     sensors, power, camera, mic, speakers
```

`sd` is the same card the Homebrew Launcher reads, so it's the easiest way to
get files in and out. `nand` and `twl` need Luma3DS extended homebrew
permissions. Without those they just don't show up.

`hw` is just files:

```sh
cat /mnt/3ds/hw/battery                          # 87
cat /mnt/3ds/hw/accel                            # 12 -4 251   (x y z)
cat /mnt/3ds/hw/info                             # summary of everything
echo "info 255 0 0" > /mnt/3ds/hw/leds           # notification LED (R G B)
cat /mnt/3ds/hw/camera_outer.rgb565 > shot.raw   # one 400x240 RGB565 frame
dd if=/mnt/3ds/hw/mic.pcm of=rec.pcm count=100   # signed 16-bit mono, 16kHz
cat music.pcm > /mnt/3ds/hw/audio.pcm            # signed 16-bit stereo, 32730Hz
```

There's also `charging`, `gyro`, `slider_3d`, `slider_volume`, `model`,
`firmware`, `region`, `language`, `steps`, `wifi`, `shell`, `adapter` and
`battery_voltage`. The sensors show up on `/dev/input/event0` as evdev axes
too.

`mic.pcm` never hits EOF, so read it with `dd count=…` instead of `cat`.
Grabbing a camera frame freezes the guest for up to a second.

## Networking

If the 3DS is on WiFi, the guest gets a DHCP lease on `10.0.2.0/24` and
outbound TCP/UDP/DNS goes out through the console. Homebrew can't open raw
sockets, so `ping` only answers from the gateway (`10.0.2.2`), and nothing can
connect in.

## Notes

Guest RAM is whatever the app can get from the 3DS heap: about 54MB on a New
3DS, about 20MB on an Old 3DS. There's another 64MB of swap on
`sdmc:/swap.img`. An Old 3DS has no spare CPU core so it's slower, and the app
drops the UI to ~8fps to leave more for Linux. The first line of the boot log
tells you which model it detected.

If you get `Image too large for RAM`, your `Image` and `3ds-cli.3dsx` are from
different releases. Update both.

## Building

The emulator core and the touch keyboard are submodules, so clone with them:

```sh
git clone --recursive https://github.com/cmdada/3DS-CLI
```

The five Nintendo consoles each need their own devkitPro toolchain, and all
of them need zlib:

```sh
dkp-pacman -S 3ds-dev wiiu-dev switch-dev wii-dev gamecube-dev
dkp-pacman -S 3ds-zlib ppc-zlib switch-zlib
```

The PSP is not a devkitPro target: i used [pspdev](https://github.com/pspdev/pspdev), Its `bin/` has to be on
`PATH` so `psp-config` is found.

```sh
make            # 3ds-cli.3dsx
make cia        # also 3ds-cli.cia (needs bannertool + makerom)
make wiiu       # dist/wiiu/3ds-cli.rpx and .wuhb
make switch     # dist/switch/3ds-cli.nro
make wii        # dist/wii/3ds-cli.dol
make gamecube   # dist/cube/3ds-cli.dol
make psp        # dist/psp/EBOOT.PBP
make dtb        # regenerate the device tree after editing source/3ds-cli.dts
```

Only the 3DS builds to the repo root; every other console writes to `dist/`,
because the binaries all keep the `3ds-cli` name and two of them are a `.dol`.

`source/core/` is the whole machine: the emulator, the virtio devices, the
terminal and the settings page. It includes no console SDK header at all.
Each console supplies `plat.c`, `plat_cfg.h`, `plat_hw.h` and `plat_kbd.h`
under `source/platform/<console>/`, against the interface in
`source/core/plat.h`. Adding a console means writing those four files.

The kernel and rootfs are built with the Buildroot tree in `buildroot/`:

```sh
wget https://buildroot.org/downloads/buildroot-2024.02.9.tar.gz
tar xf buildroot-2024.02.9.tar.gz && cd buildroot-2024.02.9
make BR2_EXTERNAL=../buildroot O=../out 3ds_defconfig
make O=../out
cd .. && python3 tools/mkimage.py out/images/Image out/images/rootfs.ext4 Image
```

Nothing under `buildroot/` is console-specific: the same kernel and rootfs
boot on all six, and the guest learns its terminal size from
`/mnt/3ds/hw/console_size`. Kernel config is
`buildroot/board/3ds-cli/linux.config`. Packages are in
`buildroot/configs/3ds_defconfig`. You can also just put a plain kernel `Image`
and a separate `rootfs.ext2` on the SD card, which is easier while iterating.


## Credits

- Built with [devkitARM / libctru](https://github.com/devkitPro/libctru), and
  with [pspdev](https://github.com/pspdev/pspdev) for the PSP
- Powered by [mini-rv32ima-mmu](https://github.com/cmdada/mini-rv32ima-mmu)
- Bottom-screen keyboard is [ctr-osk-rt](https://github.com/cmdada/ctr-osk-rt),
  which started life in this repo and is now usable in any 3DS homebrew

## License

GPL-3.0. See [LICENSE](LICENSE).
