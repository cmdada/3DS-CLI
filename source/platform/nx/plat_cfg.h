#ifndef PLAT_CFG_H
#define PLAT_CFG_H

/* Nintendo Switch (devkitA64 + libnx). See source/core/plat.h. */

#define PLAT_NAME        "Switch"

/* libnx mounts the SD card as the default device, so an unqualified path
   already lands there; the prefix is spelled out to match every other
   console's PLAT_SD and to stay independent of the current directory. */
#define PLAT_SD          "sdmc:/"

/* libnx Mutex is a u32. */
#define PLAT_MUTEX_SIZE  8

/* One 1280x720 screen, split: terminal on the top 480 rows, panel across the
   full width of the bottom 240. The panel layout is derived from its size
   (see settings.h and ada_osk_fit), so the keyboard fills the strip instead
   of sitting in the middle of it. */
#define PLAT_TERM_W      1280
#define PLAT_TERM_H      480
#define PLAT_PANEL_W     1280
#define PLAT_PANEL_H     240
#define NX_SCREEN_W      1280
#define NX_SCREEN_H      720

/* libnx framebuffers are RGBA, red first. */
#define PLAT_PIXEL_RGB

/* 1280x480 at the 8x8 font. */
#define TERM_COLS        160
#define TERM_ROWS        60
#define TERM_SCROLLBACK  200

/* An application gets ~3.2GB. RV32 lowmem is the ceiling here, not the
   console: without HIGHMEM the guest kernel runs out of linear map around
   1GB, so 512MB is comfortably inside it and far more than the guest has
   ever had. */
#define PLAT_RAM_MAX_MB  512
#define PLAT_RAM_MIN_MB  8

/* With 512MB the guest has no use for one. */
#define PLAT_WANT_SWAP   false

/* This console has a network stack; see virtio_net.h. */
#define PLAT_HAS_NET

/* ...and a TLS-capable HTTP client to go with it (devkitPro's curl
   portlib), so a card with no Image on it can be offered the download
   in machine.c rather than just an error. */
#define PLAT_HAS_DOWNLOAD

#endif /* PLAT_CFG_H */
