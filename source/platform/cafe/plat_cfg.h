#ifndef PLAT_CFG_H
#define PLAT_CFG_H

/* Nintendo Wii U (devkitPPC + wut). See source/core/plat.h. */

#define PLAT_NAME        "Wii U"

/* Aroma and the Homebrew Launcher both mount the SD card here. Unlike
   Horizon's drive prefix this is a real path, so the directory exists. */
#define PLAT_SD          "fs:/vol/external01/"

/* The Wii U's filesystem wants an aligned destination for reads; guest RAM
   offsets are not. See virtio_blk.h. */
#define PLAT_FS_NEEDS_ALIGNED_IO

/* OSScreen stores pixels as big-endian 0xRRGGBBXX, so red is the low byte. */
#define PLAT_PIXEL_RGB

/* OSMutex is 0x2c. */
#define PLAT_MUTEX_SIZE  48

/* The TV is the guest's console. */
#define PLAT_TERM_W      1280
#define PLAT_TERM_H      720

/* The GamePad is the panel, exposed as a logical 320x240 that plat_present
   pixel-doubles into the DRC's 854x480. An integer 2x lands exactly on the
   480 rows and leaves a 107px letterbox each side, so the keyboard and the
   settings page - whose layout constants are tuned to 320x240 - come across
   pixel-perfect with no rework. */
#define PLAT_PANEL_W     320
#define PLAT_PANEL_H     240
#define CAFE_PANEL_SCALE 2

/* 1280x720 at the 8x8 font. The guest reads this back from hw/console_size
   and sets its TTY to match, so a shared rootfs still fills the screen. */
#define TERM_COLS        160
#define TERM_ROWS        90
#define TERM_SCROLLBACK  200

/* An application gets ~1GB of MEM2. 256MB is far more than the guest has ever
   had and still leaves the rest of the system untouched; RV32 lowmem is the
   real ceiling above this, not the console. */
#define PLAT_RAM_MAX_MB  64
#define PLAT_RAM_MIN_MB  8

/* With 256MB the guest has no use for one. */
#define PLAT_WANT_SWAP   false

/* This console has a network stack; see virtio_net.h. */
#define PLAT_HAS_NET

/* ...and a TLS-capable HTTP client to go with it (devkitPro's curl
   portlib), so a card with no Image on it can be offered the download
   in machine.c rather than just an error. */
#define PLAT_HAS_DOWNLOAD

#endif /* PLAT_CFG_H */
