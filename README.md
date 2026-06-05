# 3DS-CLI

Run a real RISC-V Linux environment inside the Nintendo 3DS homebrew launcher.

3DS-CLI embeds [mini-rv32ima](https://github.com/cnlohr/mini-rv32ima), a compact
RISC-V CPU emulator, into a 3DS homebrew app. It boots a Linux image from the SD
card while still running inside the normal 3DS Horizon OS.

![3DS-CLI running on a Nintendo 3DS](example.jpg)

## What it does

- Boots a RISC-V Linux image on 3DS hardware through software emulation.
- Displays standard VT100-style terminal output on the top screen.
- Provides a custom bottom-screen touch keyboard for typing commands.
- Includes a prebuilt image with BusyBox tools, a JavaScript runtime, and CLI Doom.
- Can be used as a starting point for testing custom RISC-V Linux images, kernels,
  and Buildroot experiments on a 3DS.

## Installation

### Option 1: Universal Updater
Install directly on your 3DS via [Universal Updater](https://db.universal-team.net/3ds/3ds-cli) — find 3DS-CLI in the app and it will handle downloading and placing the files for you.

### Option 2: Manual
1. Download `3DS-CLI_Install.zip` from the latest GitHub release.
2. Unzip the archive.
3. Copy `3ds-cli.3dsx` and `Image` into the `3ds/` folder on your SD card.
4. Launch 3DS-CLI from the Homebrew Launcher.
5. Wait for Linux to boot. On my SDXC card it takes about 2.2 seconds to reach the
   login prompt.
6. Log in as `root` with a blank password.

## Controls

- Bottom screen: touch keyboard
- START: exit the emulator and return to the Homebrew Launcher

## Building

Install devkitPro with devkitARM and libctru, then run:

```sh
make
```

The app builds as `3ds-cli.3dsx`.


## Credits

- Built with [devkitARM / libctru](https://github.com/devkitPro/libctru)
- Powered by [mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) by cnlohr

## License

GPL-3.0. See [LICENSE](LICENSE).
