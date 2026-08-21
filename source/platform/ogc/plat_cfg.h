#ifndef PLAT_CFG_H
#define PLAT_CFG_H

/* Nintendo Wii and GameCube (devkitPPC + libogc). See source/core/plat.h.
 *
 * One backend, two consoles: libogc defines HW_RVL for the Wii, and what
 * differs is which capabilities exist rather than how anything works.
 */

#ifdef HW_RVL
#define PLAT_NAME        "Wii"
/* The front SD slot, mounted by libfat. */
#define PLAT_SD          "sd:/"
/* MEM2 is 64MB and the app has most of it. */
#define PLAT_RAM_MAX_MB  48
#else
#define PLAT_NAME        "GameCube"
/* An SD Gecko in memory card slot A. A memory card cannot hold a ~200MB
   rootfs, so there is no other option on this console. */
#define PLAT_SD          "carda:/"
/* 24MB of MEM1, less the app, the framebuffers and the terminal's scratch.
   This is below what the guest kernel needs to boot - see the size check in
   machine.c - so the GameCube is expected to report the Image as too large
   until a smaller kernel is built for it. */
#define PLAT_RAM_MAX_MB  20
#endif

#define PLAT_RAM_MIN_MB  8

#ifdef HW_RVL
/* libogc's stack prefixes its calls net_*; see source/core/plat_sock.h. The
   GameCube build has no stack to link against at all. */
#define PLAT_HAS_NET
#define PLAT_SOCK_OGC
#endif

/* Both consoles are single core, so the emulator is stepped inline and there
   is no thread to give it. */
#define PLAT_MUTEX_SIZE  8

/* 640x480, split: terminal on the top 288 rows, panel across the full width
   of the bottom 192. */
#define PLAT_TERM_W      640
#define PLAT_TERM_H      288
#define PLAT_PANEL_W     640
#define PLAT_PANEL_H     192
#define OGC_SCREEN_W     640
#define OGC_SCREEN_H     480

/* The scratch the terminal and panel are drawn into is plain BGRA; the
   framebuffer the video block scans is YUY2, and plat_present converts. */
#define PLAT_FS_NEEDS_ALIGNED_IO

/* 640x288 at the 8x8 font. */
#define TERM_COLS        80
#define TERM_ROWS        36
#define TERM_SCROLLBACK  200

/* Guest RAM here is small enough that anything substantial gets OOM-killed
   without a swap file on the card. */
#define PLAT_WANT_SWAP   true

#endif /* PLAT_CFG_H */
