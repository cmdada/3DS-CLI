#ifndef PLAT_CFG_H
#define PLAT_CFG_H

/* Nintendo 3DS (devkitARM + libctru). See source/core/plat.h. */

#define PLAT_NAME        "3DS"

/* Every path the app writes: config, the three logs, the rootfs and the swap
   file. Horizon's SD mount is a drive prefix, not a directory, so this is not
   a path that can be created if missing. */
#define PLAT_SD          "sdmc:/"

/* LightLock is an s32. Rounded up to a machine word's worth of slack so a
   libctru change doesn't silently overflow the opaque storage in plat.h;
   plat.c static_asserts the real size against it. */
#define PLAT_MUTEX_SIZE  8

/* Physical panels. The top screen is the guest console, the bottom is the
   touch keyboard and the settings page. */
#define PLAT_TERM_W      400
#define PLAT_TERM_H      240
#define PLAT_PANEL_W     320
#define PLAT_PANEL_H     240

/* The grid the guest is told about, which is larger than what fits on screen
   at zoom 1 in both axes - panning is how the rest is reached. Kept at the
   3DS's historical 80x30 so an existing rootfs sees no change. */
#define TERM_COLS        80
#define TERM_ROWS        30
#define TERM_SCROLLBACK  200

/* Guest RAM is whatever the heap yields: main() walks malloc down from the
   max in 1MB steps, and Linux needs the min left over after the kernel image
   or it panics during boot. ~54MB lands on a New3DS, ~20MB on an Old 3DS. */
#define PLAT_RAM_MAX_MB  64
#define PLAT_RAM_MIN_MB  8

/* Guest RAM here is small enough that anything substantial gets OOM-killed
   without a swap file on the card. */
#define PLAT_WANT_SWAP   true

/* This console has a network stack; see virtio_net.h. */
#define PLAT_HAS_NET

#endif /* PLAT_CFG_H */
