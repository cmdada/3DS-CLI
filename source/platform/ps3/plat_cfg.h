#ifndef PLAT_CFG_H
#define PLAT_CFG_H

/* Sony PlayStation 3 (PSL1GHT). See source/core/plat.h. */

#define PLAT_NAME        "PS3"

/* The internal drive. Unlike every other console here this is not a card the
   user can pull, so there is no "copy it on a PC" story - the files arrive
   over FTP or with the .pkg. plat_init creates the directory. */
#define PLAT_SD          "/dev_hdd0/3ds-cli/"

/* sys_lwmutex_t is 24 bytes: a u64 lock word and four u32s. The syscall
   mutex (sys_mutex_t, an s32 id) would fit in 8, but this lock is taken
   around every guest-visible device access and the lightweight one has a
   userspace fast path where the syscall one does not. */
#define PLAT_MUTEX_SIZE  32

/* Forced to 720p in plat.c rather than taken from videoGetState, because
   these are compile-time and the console's current mode is not: an SDTV user
   would otherwise get a 1280-wide surface scanned out at 640. Same split and
   the same grid as the Switch, which is the same panel size.
 */
#define PLAT_TERM_W      1280
#define PLAT_TERM_H      480
#define PLAT_PANEL_W     1280
#define PLAT_PANEL_H     240
#define PS3_SCREEN_W     1280
#define PS3_SCREEN_H     720

/* The display scans X8R8G8B8, and the PPU is big-endian, so the bytes in
   memory run X,R,G,B - red first, one byte in. plat_surface absorbs that
   offset by pointing base at the R of pixel (0,0), which is the same trick
   the 3DS uses for its rotated framebuffer. */
#define PLAT_PIXEL_RGB

/* 1280x480 at the 8x8 font. */
#define TERM_COLS        160
#define TERM_ROWS        60
#define TERM_SCROLLBACK  200

/* GameOS leaves an application ~200MB of the 256MB XDR, less the RSX command
   buffer's host region and the frame scratch. 128MB is well inside that and
   already more than every console here except the Switch; worth raising once
   it has been measured on hardware rather than guessed at. */
#define PLAT_RAM_MAX_MB  128
#define PLAT_RAM_MIN_MB  8

/* With 128MB the guest has no use for one. */
#define PLAT_WANT_SWAP   false

/* This console has a network stack; see virtio_net.h. PSL1GHT spells the
   socket calls with the BSD names but cannot close one with close(), and has
   no fcntl on sockets at all - see PLAT_SOCK_PS3 in source/core/plat_sock.h. */
#define PLAT_HAS_NET
#define PLAT_SOCK_PS3

/* No PLAT_HAS_DOWNLOAD: ps3libraries does ship curl, but it is not part of
   the toolchain the way devkitPro's portlibs are, and assuming it would make
   the build fail for anyone who installed only ps3toolchain. A missing Image
   stays an error to fix over FTP. See source/core/download.h. */

/* Precautionary rather than measured: lv2's filesystem reads want an aligned
   destination for the DMA path, and a disk read's destination is wherever in
   guest RAM the driver pointed. The bounce costs one memcpy per 32KB. Drop
   this if an unaligned read turns out to be fine on hardware. */
#define PLAT_FS_NEEDS_ALIGNED_IO

#endif /* PLAT_CFG_H */
