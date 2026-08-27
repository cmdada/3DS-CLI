#ifndef PLAT_CFG_H
#define PLAT_CFG_H

/* Sony PlayStation Portable (pspdev + pspsdk). See source/core/plat.h. */

#define PLAT_NAME        "PSP"

/* The Memory Stick */
#define PLAT_SD          "ms0:/"

/*ROUNDED UP TO AN INT THIS CAUSED SO MANY ISSUES */
#define PLAT_MUTEX_SIZE  8

/* One 480x272 screen, split: terminal on the top 160 rows, panel across the
   full width of the bottom 112.
 */
#define PLAT_TERM_W      480
#define PLAT_TERM_H      160
#define PLAT_PANEL_W     480
#define PLAT_PANEL_H     112
#define PSP_SCREEN_W     480
#define PSP_SCREEN_H     272

/* The display block scans 32bpp pixels as 0xAABBGGRR, so red is the low
   byte. The high byte is alpha, which I visceraly dislike */
#define PLAT_PIXEL_RGB

/* 80 columns at the 5x7 font is 400px and 20 rows is 160, so the whole grid
   is on screen at once with the small font and pans horizontally with the
   8x8 one */
#define TERM_COLS        80
#define TERM_ROWS        20
#define TERM_SCROLLBACK  200

/* PARAM.SFO asks for the expanded user partition (MEMSIZE=2, see mk/psp.mk)
   which puts the older model in the same bracket as an Old 3DS. */
#define PLAT_RAM_MAX_MB  48
#define PLAT_RAM_MIN_MB  8

/* A PSP-1000's share is small enough that anything substantial gets
   OOM-killed without a swap file on the card. */
#define PLAT_WANT_SWAP   true

/* This console has a network stack; see virtio_net.h. */
#define PLAT_HAS_NET

/* ...and pspdev's curl portlib is built against mbedTLS, so a card with no
   Image on it can be offered the download in machine.c rather than just an
   error. See source/core/download.h. */
#define PLAT_HAS_DOWNLOAD

#endif /* PLAT_CFG_H */
