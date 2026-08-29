#include <zlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>   /* ftruncate, for preallocating the extracted rootfs */
#include "plat.h"
#include "terminal.h"
#include "theme.h"    /* the adabit palette, shared by the terminal and the UI */
#include "config.h"   /* <SD>/3ds-cli.cfg                                      */
#include "ui.h"       /* bottom-screen drawing primitives                     */
#include "download.h" /* fetching a missing Image from the GitHub release      */

TermState term_state;

/* Live settings. The device flags in here are deliberately NOT what the
   emulator reads - see the g_dev_* snapshot below. */
static Cfg g_cfg;


static void term_printf(const char *format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  
  char *s = buf;
  while (*s) {
    term_write_char(&term_state, *s++);
  }
}



// ---------------------------------------------------------
// mini-rv32ima Integration
// ---------------------------------------------------------
#include "default64mbdtc.h"

uint32_t ram_amt = PLAT_RAM_MAX_MB * 1024u * 1024u;
// Smallest amount of RAM left over after the kernel image that's worth
// trying to boot on - see the size check in main().
#define MIN_GUEST_FREE_RAM (PLAT_RAM_MIN_MB * 1024u * 1024u)
uint8_t *ram_image = 0;
struct MiniRV32IMAState *core;

#define EMU_RUN_BUDGET_US 50000
// Instructions per MiniRV32IMAStep call. This also sets the resolution of the
// guest's clock (see the tick handling in EmuStepBatch), so it trades
// timekeeping accuracy against throughput - and the trade is steeper than it
// looks. Dropping this to 20000, for a ~10ms guest clock instead of ~100ms,
// cost 5.6% throughput and raised the guest's own retired-instruction count
// for the same workload by 8.5%: at 100Hz the kernel then actually takes ten
// times as many timer interrupts, each a full trap round-trip through the
// SBI forwarding in HandleException. Left coarse deliberately.
#define EMU_STEP_CHUNK 200000

/* How often the main thread redraws the terminal and polls input. The values
   come from plat_ui_cadence() at startup, which is where the reasoning about
   sharing a core with the emulator lives. */
static uint32_t g_top_refresh_us;
static uint32_t g_input_poll_us;
// How often the guest console mirror on the SD card is actually committed.
// See WriteUARTByte for why this is time-based rather than per-line.
#define UART_LOG_FLUSH_INTERVAL_US 500000

char rx_buf[256];
int rx_head = 0, rx_tail = 0;

// Guards everything shared between the main (input/render) thread and the
// emulation thread: the rx_buf keyboard-input ring below, and term_state's
// emu-owned fields (grid/cursor/scrollback - written by term_write_char on
// the emu thread, read by term_draw on the main thread). term_state's
// main-thread-owned fields (zoom/scroll/auto_track/use_5x7) are read and
// written only ever from the main thread and don't need this lock - see
// the PresentTopScreen/WriteUARTByte call sites below for what actually
// needs to be inside it.
static plat_mutex_t ui_lock;

static int IsKBHit() {
  plat_mutex_lock(&ui_lock);
  int r = rx_head != rx_tail;
  plat_mutex_unlock(&ui_lock);
  return r;
}
static int ReadKBByte() {
  plat_mutex_lock(&ui_lock);
  int result = -1;
  if (rx_head != rx_tail) {
    result = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % 256;
  }
  plat_mutex_unlock(&ui_lock);
  return result;
}
void rx_push(char c) {
  plat_mutex_lock(&ui_lock);
  int nh = (rx_head + 1) % 256;
  if (nh != rx_tail) { rx_buf[rx_head] = c; rx_head = nh; }
  plat_mutex_unlock(&ui_lock);
}
void rx_push_str(const char *s) { while (*s) rx_push(*s++); }

static uint32_t uart_byte_count = 0;
static FILE *uart_log_file = NULL; // <SD>/3ds-cli-console.log mirror of guest console output

/* newlib gives a stream its buffer lazily, on the first read or write, and
   sizes it by fstat()ing the descriptor. Neither half of that is wanted here.
   The size it picks is a guess with nothing to do with how an SD card likes to
   be written, and the log below is filled a byte at a time by fputc, so the
   buffer is exactly what decides how many card writes a boot costs. The fstat
   is worse: on the Wii U it goes through wut's FSA layer, where the first byte
   of guest console output brings the whole app down inside __wut_fsa_fstat - a
   crash that only shows up once there is an Image to boot, because without one
   the guest never prints. Handing each stream its buffer up front settles
   both. */
#define LOG_BUF_SIZE 4096
static char uart_log_buf[LOG_BUF_SIZE];
static char dbg_log_buf[LOG_BUF_SIZE];

static void SetStreamBuffer(FILE *f, char *buf, size_t len) {
  /* Must precede any I/O on the stream, or newlib has already made one. */
  if (f) setvbuf(f, buf, _IOFBF, len);
}
static void WriteUARTByte(char c) {
  uart_byte_count++;
  plat_mutex_lock(&ui_lock);
  /* Gated, or scrollback is unreadable while the guest is printing: every
     byte would snap the viewport back. "Follow output", also bound to ZL. */
  if (g_cfg.follow_output) term_state.auto_track = true;
  term_write_char(&term_state, c);
  plat_mutex_unlock(&ui_lock);
  // File I/O below is safe unlocked: uart_log_file is only ever touched
  // from whichever single thread calls WriteUARTByte (the emu thread once
  // one exists), never concurrently from two threads.
  if (uart_log_file) {
    fputc(c, uart_log_file);
    // Flushing on every newline meant one SD card write per line of guest
    // console output - several hundred over a boot, each one stalling this
    // (the emulation) thread on the card for as long as the write takes. The
    // reason to flush at all is that the log survives a crash or a battery
    // pull, and a time-based flush keeps essentially all of that value: at
    // most UART_LOG_FLUSH_INTERVAL_US of output is ever at risk, for a small
    // fraction of the writes. The stream is still closed explicitly on exit,
    // so a clean shutdown loses nothing at all.
    if (c == '\n') {
      static uint64_t last_flush_tick = 0;
      uint64_t now = plat_us();
      if (now - last_flush_tick >= UART_LOG_FLUSH_INTERVAL_US) {
        fflush(uart_log_file);
        last_flush_tick = now;
      }
    }
  }
}

static bool TimeSinceUs(uint64_t last_us, uint64_t interval_us) {
  return plat_us() - last_us >= interval_us;
}

static void PresentTopScreen(uint64_t *last_present_us) {
  plat_fb_t fb;
  if (!plat_surface(PLAT_SURF_TERM, &fb)) return;
  // Locked only around the part that actually touches term_state/reads the
  // grid the emu thread writes - plat_present blocks for vsync and must not
  // hold this lock while doing so, or a burst of guest console output would
  // stall behind a whole frame's wait for no reason.
  plat_mutex_lock(&ui_lock);
  term_draw(&term_state, &fb);
  term_state.dirty = false;
  plat_mutex_unlock(&ui_lock);
  plat_present(PLAT_SURF_BIT(PLAT_SURF_TERM));
  *last_present_us = plat_us();
}

static FILE *OpenDiskFile(const char **opened_path) {
  static const char *paths[] = { PLAT_SD "rootfs.ext2", "rootfs.ext2" };
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    FILE *f = fopen(paths[i], "r+b");
    if (f) {
      if (opened_path) *opened_path = paths[i];
      return f;
    }
  }
  if (opened_path) *opened_path = paths[0];
  return NULL;
}

/* The Image file may be a combined bundle produced by tools/mkimage.py:
   [kernel][gzipped rootfs][u32 gz_len][u32 raw_len]["3DSCLIRF"]. Detect
   the 16-byte trailer and return the kernel-only length (or the full file
   length for a plain kernel Image, which stays fully supported). */
#define IMAGE_TRAILER_MAGIC "3DSCLIRF"
static long ImageKernelLen(FILE *f, long flen, uint32_t *gz_len_out, uint32_t *raw_len_out) {
  uint8_t tr[16];
  if (flen <= 16) return flen;
  fseek(f, flen - 16, SEEK_SET);
  if (fread(tr, sizeof(tr), 1, f) != 1) { fseek(f, 0, SEEK_SET); return flen; }
  fseek(f, 0, SEEK_SET);
  if (memcmp(tr + 8, IMAGE_TRAILER_MAGIC, 8) != 0) return flen;
  uint32_t gz_len, raw_len;
  memcpy(&gz_len, tr + 0, 4);
  memcpy(&raw_len, tr + 4, 4);
  if ((long)gz_len + 16 >= flen) return flen; /* implausible: treat as plain */
  if (gz_len_out) *gz_len_out = gz_len;
  if (raw_len_out) *raw_len_out = raw_len;
  return flen - 16 - (long)gz_len;
}

/* One-time first-boot extraction of the embedded rootfs to the SD card,
   where it lives as the writable, persistent filesystem from then on. */
static bool ExtractEmbeddedRootfs(FILE *f, long gz_off, uint32_t gz_len, uint32_t raw_len,
                                  uint64_t *present_tick) {
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, 15 + 16 /* gzip wrapper */) != Z_OK) return false;

  /* Inflate to a scratch name and rename only once the whole thing is on the
     card. Writing straight to rootfs.ext2 meant an extraction that never got
     to finish - the user quits, the battery dies, the console is closed -
     left a truncated file behind, and because OpenDiskFile only checks that
     the path opens, every later launch would happily mount the half of a
     filesystem that made it, with no way for the user to tell what had
     happened. A leftover .part is inert: nothing opens it, and the next
     attempt truncates it. */
  static const char *kRootfsPath = PLAT_SD "rootfs.ext2";
  static const char *kRootfsPart = PLAT_SD "rootfs.ext2.part";
  FILE *out = fopen(kRootfsPart, "wb");
  if (!out) { inflateEnd(&zs); return false; }

  /* Set the final length up front rather than growing the file 256KB at a
     time for ~200MB. Extending a file makes the FS walk and extend its FAT
     cluster chain, which gets more expensive the longer the chain already
     is, so the incremental version got slower the further it went - the
     first-boot extraction was running at a fraction of the card's actual
     write speed and taking many minutes. Best-effort: if the FS declines,
     the writes below still work, they're just back to being slow. */
  if (ftruncate(fileno(out), (off_t)raw_len) == 0) rewind(out);

  static uint8_t inbuf[64 * 1024], outbuf[256 * 1024];
  uint32_t in_left = gz_len, done = 0, last_mb = 0;
  bool ok = true;
  fseek(f, gz_off, SEEK_SET);
  term_printf("First boot: extracting rootfs.ext2 (%luMB) to SD...\n",
              (unsigned long)(raw_len >> 20));
  PresentTopScreen(present_tick);

  int zret = Z_OK;
  while (zret != Z_STREAM_END) {
    if (zs.avail_in == 0 && in_left > 0) {
      uint32_t n = in_left < sizeof(inbuf) ? in_left : sizeof(inbuf);
      if (fread(inbuf, n, 1, f) != 1) { ok = false; break; }
      zs.next_in = inbuf; zs.avail_in = n; in_left -= n;
    }
    zs.next_out = outbuf; zs.avail_out = sizeof(outbuf);
    zret = inflate(&zs, Z_NO_FLUSH);
    if (zret != Z_OK && zret != Z_STREAM_END) { ok = false; break; }
    uint32_t produced = sizeof(outbuf) - zs.avail_out;
    if (produced && fwrite(outbuf, produced, 1, out) != 1) { ok = false; break; }
    done += produced;
    if ((done >> 20) >= last_mb + 16) {
      last_mb = done >> 20;
      term_printf("  ... %luMB / %luMB\n", (unsigned long)last_mb, (unsigned long)(raw_len >> 20));
      PresentTopScreen(present_tick);
    }
  }
  inflateEnd(&zs);
  fclose(out);
  fseek(f, 0, SEEK_SET);
  if (!ok || done != raw_len) {
    term_printf("Extraction failed, removing partial rootfs.\n");
    remove(kRootfsPart);
    return false;
  }
  /* rename() onto an existing name fails on FAT, and this is reachable with
     rootfs.ext2 already there: OpenDiskFile only failed to *open* it, which
     a zero-byte or otherwise broken leftover would also do. */
  remove(kRootfsPath);
  if (rename(kRootfsPart, kRootfsPath) != 0) {
    term_printf("Could not finalize rootfs.ext2 on the SD card.\n");
    remove(kRootfsPart);
    return false;
  }
  term_printf("Rootfs extracted.\n");
  PresentTopScreen(present_tick);
  return true;
}

#ifdef PLAT_HAS_DOWNLOAD

/* What a RISC-V Linux Image says it is: "RISCV" at 0x30 on 4.15+, and the
   0x38 magic2 that superseded it in 5.5. With no certificate checking on the
   download (see download.h), this is what keeps a captive portal's login page
   - or a transfer that ended early on a 200 - from being kept as `Image` and
   failing much later as an illegal instruction with nothing to point at. */
static bool LooksLikeKernelImage(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  uint8_t hdr[64];
  bool ok = fread(hdr, sizeof(hdr), 1, f) == 1 &&
            (memcmp(hdr + 0x38, "RSC\x05", 4) == 0 ||
             memcmp(hdr + 0x30, "RISCV", 5) == 0);
  fclose(f);
  return ok;
}

typedef struct {
  uint64_t *present_tick;
  uint32_t  last_mb;
} DlUi;

/* Progress in the same shape as the first-boot rootfs extraction - a line
   every few MB, not a bar, because the terminal is the only surface there is
   this early. It doubles as the input poll: curl blocks for the whole
   transfer, so this is the app's only chance to repaint, to see B, or to
   notice the OS asking it to go away. */
static bool DownloadProgress(uint64_t got, uint64_t total, void *ctx) {
  DlUi *ui = (DlUi *)ctx;
  if (!plat_running()) return false;

  plat_input_t in;
  plat_poll_input(&in);
  if (in.down & (PLAT_BTN_B | PLAT_BTN_QUIT)) return false;

  uint32_t mb = (uint32_t)(got >> 20);
  if (mb >= ui->last_mb + 4) {
    ui->last_mb = mb;
    if (total)
      term_printf("  ... %luMB / %luMB\n", (unsigned long)mb,
                  (unsigned long)(total >> 20));
    else
      term_printf("  ... %luMB\n", (unsigned long)mb);
    PresentTopScreen(ui->present_tick);
  }
  return true;
}

/* Offered when the card has no Image: without one there is nothing to boot,
   and the alternative is an error telling the user to go and find a PC. True
   means an Image is now on the card and the caller should open it. */
static bool PromptDownloadImage(uint64_t *present_tick) {
  term_printf("No Image found on the SD card.\n");
  term_printf("Press A to download it (~55MB) from the\n"
              "latest release, or B to quit.\n");
  PresentTopScreen(present_tick);

  for (;;) {
    if (!plat_running()) return false;
    plat_input_t in;
    plat_poll_input(&in);
    if (in.down & PLAT_BTN_A) break;
    if (in.down & (PLAT_BTN_B | PLAT_BTN_QUIT)) return false;
    plat_sleep_us(g_input_poll_us);
  }

  term_printf("Bringing up the network...\n");
  PresentTopScreen(present_tick);
  if (!plat_net_init()) {
    term_printf("Network unavailable.\n");
    PresentTopScreen(present_tick);
    return false;
  }

  term_printf("Downloading Image...\n");
  PresentTopScreen(present_tick);

  DlUi ui = { present_tick, 0 };
  bool ok = dl_fetch(DL_IMAGE_URL, PLAT_SD "Image", DownloadProgress, &ui);

  /* Handed straight back rather than kept for the guest: vnet_init brings the
     stack up again if the guest wants networking at all, and on the 3DS the
     SOC session holds a megabyte that the RAM walk in main() would otherwise
     never get to offer the guest. */
  plat_net_exit();

  if (ok && !LooksLikeKernelImage(PLAT_SD "Image")) {
    term_printf("What downloaded is not a kernel Image.\n");
    remove(PLAT_SD "Image");
    ok = false;
  }
  if (ok) term_printf("Image downloaded.\n");
  else    term_printf("Download failed.\n");
  PresentTopScreen(present_tick);
  return ok;
}

#else

/* No TLS on this console, so a missing Image is simply an error. */
static inline bool PromptDownloadImage(uint64_t *present_tick) {
  (void)present_tick;
  return false;
}

#endif /* PLAT_HAS_DOWNLOAD */

/* Backing store for the guest's swap device (/dev/vdb). Guest RAM is only
   whatever the 3DS heap could spare (8-64MB, see the malloc loop in main),
   so this is what keeps anything nontrivial from being OOM-killed.
   Created fresh each launch and removed on exit, so it never becomes a
   stale file the user has to know about.

   Note this is deliberately *not* zero-filled: the file is created by
   seeking to the last byte and writing there, which costs nothing beyond
   allocating the space. Swap only ever reads back pages it wrote itself,
   so whatever the filesystem leaves in the gap is never observed. Some SD
   filesystems don't extend a file that way, so the result is verified and
   falls back to an explicit (slow, one-time) zero-fill. */
#define SWAP_PATH        PLAT_SD "swap.img"
#define SWAP_SIZE_BYTES  (64u * 1024u * 1024u)

static FILE *CreateSwapFile(uint64_t *present_tick) {
  FILE *f = fopen(SWAP_PATH, "w+b");
  if (!f) return NULL;

  if (fseek(f, (long)SWAP_SIZE_BYTES - 1, SEEK_SET) == 0 && fputc(0, f) != EOF) {
    fflush(f);
    fseek(f, 0, SEEK_END);
    if ((uint64_t)ftell(f) == SWAP_SIZE_BYTES) { rewind(f); return f; }
  }

  /* Sparse extend didn't take: write the whole thing out, with progress,
     the same way the first-boot rootfs extraction reports itself. Small
     stack buffer rather than a static one - this path is rare, SD
     throughput dominates the loop anyway, and any permanent buffer here
     comes straight out of the heap the guest's RAM is carved from. */
  uint8_t zeros[4096];
  memset(zeros, 0, sizeof(zeros));
  rewind(f);
  term_printf("Creating swap file (%luMB)...\n", (unsigned long)(SWAP_SIZE_BYTES >> 20));
  PresentTopScreen(present_tick);
  uint32_t written = 0, last_mb = 0;
  while (written < SWAP_SIZE_BYTES) {
    uint32_t n = SWAP_SIZE_BYTES - written;
    if (n > sizeof(zeros)) n = sizeof(zeros);
    if (fwrite(zeros, n, 1, f) != 1) {
      fclose(f);
      remove(SWAP_PATH);
      return NULL;
    }
    written += n;
    if ((written >> 20) >= last_mb + 16) {
      last_mb = written >> 20;
      term_printf("  ... %luMB / %luMB\n", (unsigned long)last_mb,
                  (unsigned long)(SWAP_SIZE_BYTES >> 20));
      PresentTopScreen(present_tick);
    }
  }
  fflush(f);
  rewind(f);
  return f;
}

static FILE *dbg_log_file = NULL;

/* File-scope so every exit path (including the `goto wait_exit`s that fire
   before it's even created) can clean it up unconditionally. */
static FILE *swap_file = NULL;

static void CloseSwapFile(void) {
  if (swap_file) { fclose(swap_file); swap_file = NULL; }
  remove(SWAP_PATH);
}

static uint32_t HandleException(uint32_t ir, uint32_t retval);
static uint32_t HandleControlStore(uint32_t addy, uint32_t val);
static uint32_t HandleControlLoad(uint32_t addy);
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value);
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno);

/* Guest memory is little-endian; the host may not be. See plat.h - these are
   the identity on a little-endian console and compile to byte-reversed loads
   and stores on a big-endian one. */
#define MINIRV32_CUSTOM_MEMORY_BUS

#define MINIRV32_STORE4(ofs, val) g_st32(image + (ofs), (uint32_t)(val))
#define MINIRV32_STORE2(ofs, val) g_st16(image + (ofs), (uint16_t)(val))
#define MINIRV32_STORE1(ofs, val) (*(uint8_t *)(image + (ofs)) = (uint8_t)(val))
#define MINIRV32_LOAD4(ofs) g_ld32(image + (ofs))
#define MINIRV32_LOAD2(ofs) g_ld16(image + (ofs))
#define MINIRV32_LOAD1(ofs) (*(uint8_t *)(image + (ofs)))
#define MINIRV32_LOAD2_SIGNED(ofs) ((int16_t)g_ld16(image + (ofs)))
#define MINIRV32_LOAD1_SIGNED(ofs) (*(int8_t *)(image + (ofs)))

#define MINIRV32WARN(x...) printf(x);
#define MINIRV32_DECORATE static
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC(pc, ir, retval) { if (retval > 0) retval = HandleException(ir, retval); }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) if (HandleControlStore(addy, val)) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) rval = HandleControlLoad(addy);
#define MINIRV32_OTHERCSR_WRITE(csrno, value) HandleOtherCSRWrite(image, csrno, value);
#define MINIRV32_OTHERCSR_READ(csrno, value) value = HandleOtherCSRRead(image, csrno);

#include "mini-rv32ima.h"
#include "plic.h"
#include "virtio_blk.h"
#include "virtio_rng.h"
#include "virtio_net.h"
#include "virtio_9p.h"
#include "virtio_input.h"
#include "rtc_goldfish.h"

/* Which devices this launch brought up, snapshotted from g_cfg in main()
   before the emulation thread starts. The MMIO decode below reads these and
   never g_cfg, which the settings page writes from the other thread - so a
   device cannot stop answering after its driver has bound to it, which would
   hang the guest. */
static bool g_dev_net = true, g_dev_rng = true, g_dev_9p = true, g_dev_input = true;

/* Included here rather than up with terminal.h: both reach into term_state,
   ui_lock, the panel keyboard, rx_push_str and the virtio devices above. */
#include "plat_kbd.h"
#include "settings.h"

// Written by the emu thread (HandleSBICall, on an SRST call), read by the
// main thread's exit check below - volatile so the main thread's compiled
// loop actually re-reads it each iteration instead of possibly hoisting a
// cached value out, since nothing in that loop's own code ever writes it.
static volatile int sbi_shutdown_requested = 0;

// ---------------------------------------------------------
// Emulation thread
// ---------------------------------------------------------
//
// Runs MiniRV32IMAStep continuously on its own thread (see main() for how
// it's spawned - New3DS core 2 preferred, system core 1 as a universal
// fallback) instead of interleaving batches of it with input/render on a
// single thread as before. This thread, and only this thread, ever touches
// `core`, `ram_image`, or any virtio/PLIC device state - that's what makes
// this safe without a much bigger lock around all of memory: the main
// thread's only interaction with anything this thread owns is through the
// two locked handoffs below (g_vinput_axes for sensor samples in, g_emu_ret
// for exit-status out) plus the ui_lock-guarded rx_buf/term_state already
// covered in WriteUARTByte/PresentTopScreen/rx_push et al.
volatile bool g_emu_rebase_clock = false;       // backend -> emu thread: the process was suspended.
static volatile bool g_emu_should_stop = false; // main thread -> emu thread: please stop.
static volatile int  g_emu_ret = 0;             // emu thread -> main thread: last MiniRV32IMAStep result.

// Written by the main thread (which owns plat_poll_input and everything
// that depends on its cache - see plat_sample_axes), read by the emu thread. Guarded by ui_lock even though
// it's a plain array with no pointers (so a torn read could at worst show
// one axis a poll cycle stale, never anything unsafe) - the lock is cheap
// and this makes that reasoning unnecessary to rely on.
static int32_t g_vinput_axes[VI_NAXES];

// Runs one batch: a chunk-loop of MiniRV32IMAStep calls up to
// EMU_RUN_BUDGET_US of wall time or a stopping condition, then services
// the pollable devices. Returns the same result codes MiniRV32IMAStep does
// (1 = WFI, 0x5555/3 = shutdown/fault sentinels used by HandleControlStore
// and the fault path respectively - see their call sites).
//
// dbg_log_file below writes a ring buffer of the last DBG_RING_N distinct
// (pc,mcause) transitions plus a full register dump to
// <SD>/3ds-cli-debug.log whenever execution is detected as stuck (200
// consecutive chunks with zero forward progress), re-arming once forward
// progress resumes. Cheap enough to leave enabled, and it has paid for
// itself repeatedly when a guest-side hang needed diagnosing.
static int EmuStepBatch(uint64_t *last_tick) {
  int ret = 0;
  uint64_t now = plat_us();
  uint64_t frame_start = now;
  static uint32_t dbg_step = 0;
  static uint32_t dbg_last_pc = 0xffffffffu, dbg_last_mc = 0xffffffffu;
  static uint32_t dbg_stuck_count = 0;
  static bool dbg_dumped = false;
#define DBG_RING_N 512
  typedef struct { uint32_t step, pc, mc, ep, tv, ra, sp, tp, t4, gp, a0, mstatus; } dbg_rec_t;
  static dbg_rec_t dbg_ring[DBG_RING_N];
  static int dbg_ring_pos = 0;
  do {
    /* Advance the guest clock once per chunk, by however much real time has
       actually passed, rather than once per batch with every chunk after the
       first told that zero time elapsed.

       The guest's clock is what rdtime reads, and RISC-V Linux implements
       udelay by spinning on rdtime. With the clock frozen for a whole chunk,
       any such delay ran to the end of that chunk no matter how short it
       asked for. This costs no extra syscalls - the tick read that the loop
       condition below already performs is reused as this iteration's
       timestamp. */
    /* Horizon suspends this process while the system keyboard applet is up,
       so on the way out the tick delta below is the whole typing session
       rather than a chunk. Handing the guest that in one step means a backlog
       of timer interrupts and an RCU stall splat; swallow the gap instead. */
    if (g_emu_rebase_clock) { g_emu_rebase_clock = false; *last_tick = now; }

    uint32_t step_us = (uint32_t)(now - *last_tick);
    *last_tick = now;
    ret = MiniRV32IMAStep(core, ram_image, 0, step_us, EMU_STEP_CHUNK);
    dbg_step++;
    bool changed = core->pc != dbg_last_pc || core->mcause != dbg_last_mc;
    if (changed) {
      dbg_ring[dbg_ring_pos].step = dbg_step;
      dbg_ring[dbg_ring_pos].pc = core->pc;
      dbg_ring[dbg_ring_pos].mc = core->mcause;
      dbg_ring[dbg_ring_pos].ep = core->mepc;
      dbg_ring[dbg_ring_pos].tv = core->mtval;
      dbg_ring[dbg_ring_pos].ra = core->regs[1];
      dbg_ring[dbg_ring_pos].sp = core->regs[2];
      dbg_ring[dbg_ring_pos].gp = core->regs[3];
      dbg_ring[dbg_ring_pos].tp = core->regs[4];
      dbg_ring[dbg_ring_pos].a0 = core->regs[10];
      dbg_ring[dbg_ring_pos].t4 = core->regs[29];
      dbg_ring[dbg_ring_pos].mstatus = core->mstatus;
      dbg_ring_pos = (dbg_ring_pos + 1) % DBG_RING_N;
      dbg_stuck_count = 0;
      dbg_dumped = false; // Forward progress resumed: re-arm for the next stall.
    } else {
      dbg_stuck_count++;
    }
    if (dbg_step <= 5 || dbg_step == 10 || dbg_step % 500 == 0 || changed) {
      /* Deliberately NOT printed to the terminal: this shares the top
         screen with the guest kernel's own console output, and spamming
         it here was scrolling away real kernel boot/panic text before
         it could ever be seen. File-only (dbg_log_file below). */
      dbg_last_pc = core->pc;
      dbg_last_mc = core->mcause;
    }
    /* Once the same (pc,mcause) has been the outcome of ~200 consecutive
       chunk calls with zero forward progress, we're permanently stuck:
       dump the ring buffer of the last DBG_RING_N distinct transitions
       (i.e. the run-up to the freeze) plus a full register snapshot. */
    if (!dbg_dumped && dbg_stuck_count == 200 && dbg_log_file) {
      dbg_dumped = true;
      fprintf(dbg_log_file, "=== STUCK at step %lu, dumping last %d transitions ===\n",
        (unsigned long)dbg_step, DBG_RING_N);
      for (int i = 0; i < DBG_RING_N; i++) {
        dbg_rec_t *r = &dbg_ring[(dbg_ring_pos + i) % DBG_RING_N];
        if (r->step == 0) continue;
        fprintf(dbg_log_file, "[%lu] pc=%08lx mc=%lx ep=%08lx tv=%08lx ra=%08lx sp=%08lx gp=%08lx tp=%08lx a0=%08lx t4=%08lx mstatus=%08lx\n",
          (unsigned long)r->step, (unsigned long)r->pc, (unsigned long)r->mc,
          (unsigned long)r->ep, (unsigned long)r->tv, (unsigned long)r->ra,
          (unsigned long)r->sp, (unsigned long)r->gp, (unsigned long)r->tp,
          (unsigned long)r->a0, (unsigned long)r->t4, (unsigned long)r->mstatus);
      }
      fprintf(dbg_log_file, "=== FULL REGDUMP at step %lu ===\n", (unsigned long)dbg_step);
      for (int ri = 0; ri < 32; ri++) {
        fprintf(dbg_log_file, "x%-2d=%08lx%s", ri, (unsigned long)core->regs[ri], (ri % 4 == 3) ? "\n" : "  ");
      }
      fprintf(dbg_log_file, "\npc=%08lx mepc=%08lx mtval=%08lx mcause=%08lx mscratch=%08lx mtvec=%08lx mstatus=%08lx mie=%08lx mip=%08lx\n",
        (unsigned long)core->pc, (unsigned long)core->mepc, (unsigned long)core->mtval,
        (unsigned long)core->mcause, (unsigned long)core->mscratch, (unsigned long)core->mtvec,
        (unsigned long)core->mstatus, (unsigned long)core->mie, (unsigned long)core->mip);
      fprintf(dbg_log_file, "satp=%08lx sepc=%08lx stval=%08lx scause=%08lx stvec=%08lx sscratch=%08lx priv=%lu\n",
        (unsigned long)core->satp, (unsigned long)core->sepc, (unsigned long)core->stval,
        (unsigned long)core->scause, (unsigned long)core->stvec, (unsigned long)core->sscratch,
        (unsigned long)(core->extraflags & 3));
      fprintf(dbg_log_file, "=== END ===\n");
      fflush(dbg_log_file);
    }
    now = plat_us();
  } while (ret != 1 &&
           ret != 0x5555 &&
           ret != 3 &&
           !sbi_shutdown_requested &&
           now - frame_start < EMU_RUN_BUDGET_US);

  vnet_poll(ram_image);

  int32_t axes[VI_NAXES];
  plat_mutex_lock(&ui_lock);
  memcpy(axes, g_vinput_axes, sizeof(axes));
  plat_mutex_unlock(&ui_lock);
  vinput_poll(ram_image, axes);

  return ret;
}

static void EmuThreadEntry(void *arg) {
  (void)arg;
  uint64_t last_tick = plat_us();
  while (!g_emu_should_stop) {
    int ret = EmuStepBatch(&last_tick);
    g_emu_ret = ret;
    if (ret == 0x5555 || ret == 3 || sbi_shutdown_requested) break;
    if (ret == 1) plat_sleep_us(1000);
  }
}

// This core boots straight into S-mode with no real M-mode firmware ever
// running (see vendor/mini-rv32ima-mmu's file header for why), so this
// hook has to act as that firmware itself for the two things a NOMMU-less
// MMU'd Linux kernel expects M-mode to service: SBI ecalls, and forwarding
// the real machine timer interrupt to the guest as a supervisor timer
// interrupt (there's no Sstc-style hardware STIP wire to do that for us).
static void HandleSBICall(void) {
  uint32_t eid = core->regs[17]; // a7
  uint32_t fid = core->regs[16]; // a6
  uint32_t a0 = core->regs[10];
  uint32_t a1 = core->regs[11];
  uint32_t ret_err = 0, ret_val = 0;

  switch (eid) {
    case 0x10: // Base extension
      switch (fid) {
        case 0: ret_val = 0x1000000; break; // get_spec_version: v1.0
        case 1: ret_val = 0xff; break;       // get_impl_id: unregistered/"other"
        case 2: ret_val = 1; break;          // get_impl_version
        case 3: // probe_extension
          switch (a0) {
            case 0x10: case 0x54494D45: case 0x4442434E: case 0x53525354:
            case 0: case 1: case 8:
              ret_val = 1; break;
            default: ret_val = 0; break;
          }
          break;
        case 4: case 5: case 6: ret_val = 0; break; // mvendorid/marchid/mimpid
        default: ret_err = (uint32_t)-2; break; // SBI_ERR_NOT_SUPPORTED
      }
      break;
    case 0x54494D45: // TIME extension
      if (fid == 0) { // set_timer(a0=stime_lo, a1=stime_hi)
        core->timermatchl = a0;
        core->timermatchh = a1;
        core->mip &= ~(1u << 5); // Clear STIP: we're (re)scheduling.
      } else ret_err = (uint32_t)-2;
      break;
    case 0x4442434E: // DBCN debug console (preferred over the legacy console calls below)
      if (fid == 2) { // console_write_byte(a0=byte)
        WriteUARTByte((char)a0);
      } else if (fid == 0) { // console_write(a0=num_bytes, a1=addr_lo)
        uint32_t ofs = a1 - MINIRV32_RAM_IMAGE_OFFSET;
        for (uint32_t i = 0; i < a0 && ofs + i < ram_amt; i++) WriteUARTByte((char)ram_image[ofs + i]);
        ret_val = a0;
      } else if (fid == 1) { // console_read - no input support, report 0 bytes read.
        ret_val = 0;
      } else ret_err = (uint32_t)-2;
      break;
    case 1: // Legacy console_putchar(a0=char)
      WriteUARTByte((char)a0);
      break;
    case 2: // Legacy console_getchar
      ret_val = IsKBHit() ? (uint32_t)(uint8_t)ReadKBByte() : (uint32_t)-1;
      break;
    case 0x53525354: // SRST system reset
      sbi_shutdown_requested = 1;
      break;
    default:
      ret_err = (uint32_t)-2;
      break;
  }

  // Legacy extensions (0-8) return their value directly in a0; everything
  // from the base extension onward uses the (a0=error, a1=value) convention.
  if (eid <= 8) core->regs[10] = ret_val;
  else { core->regs[10] = ret_err; core->regs[11] = ret_val; }
}

static uint32_t HandleException(uint32_t ir, uint32_t retval) {
  if (retval == 0x80000007) {
    // Real machine timer interrupt: forward it to the guest as a
    // supervisor timer interrupt instead of taking a (nonexistent)
    // M-mode trap.
    core->mip |= (1u << 5); // STIP
    return 0;
  }
  if (retval == (9 + 1)) { // ECALL from S-mode == SBI call, never delegated.
    HandleSBICall();
    return 0;
  }
  return retval;
}
static uint32_t HandleControlStore(uint32_t addy, uint32_t val) {
  vblk_dev_t *bd;
  if (addy == 0x10000000) WriteUARTByte((char)val);
  else if (addy == 0x11004004) core->timermatchh = val;
  else if (addy == 0x11004000) core->timermatchl = val;
  else if (addy == 0x11100000) { core->pc += 4; return val; }
  else if ((bd = vblk_for_addr(addy)) != NULL)
    vblk_store(bd, addy, val, ram_image);
  /* A device turned off is simply not decoded: the range falls through to the
     final return, so a read of its VREG_MAGIC gives 0 instead of "virt" and
     Linux's virtio-mmio driver skips the node. */
  else if (g_dev_net && addy >= VIRTIO_NET_BASE && addy < VIRTIO_NET_BASE + VIRTIO_NET_SIZE)
    vnet_store(addy, val, ram_image);
  else if (g_dev_rng && addy >= VIRTIO_RNG_BASE && addy < VIRTIO_RNG_BASE + VIRTIO_RNG_SIZE)
    vrng_store(addy, val, ram_image);
  else if (g_dev_9p && addy >= VIRTIO_9P_BASE && addy < VIRTIO_9P_BASE + VIRTIO_9P_SIZE)
    v9p_store(addy, val, ram_image);
  else if (g_dev_input && addy >= VIRTIO_INPUT_BASE && addy < VIRTIO_INPUT_BASE + VIRTIO_INPUT_SIZE)
    vinput_store(addy, val, ram_image);
  else if (addy >= RTC_GOLDFISH_BASE && addy < RTC_GOLDFISH_BASE + RTC_GOLDFISH_SIZE)
    rtc_goldfish_store(addy, val);
  else if (addy >= PLIC_BASE && addy < PLIC_BASE + PLIC_SIZE)
    plic_store(addy, val);
  return 0;
}
static uint32_t HandleControlLoad(uint32_t addy) {
  vblk_dev_t *bd;
  if (addy == 0x10000005) return 0x60 | IsKBHit();
  else if (addy == 0x10000000 && IsKBHit()) return ReadKBByte();
  else if (addy == 0x1100bffc) return core->timerh;
  else if (addy == 0x1100bff8) return core->timerl;
  else if ((bd = vblk_for_addr(addy)) != NULL)
    return vblk_load(bd, addy);
  else if (g_dev_net && addy >= VIRTIO_NET_BASE && addy < VIRTIO_NET_BASE + VIRTIO_NET_SIZE)
    return vnet_load(addy);
  else if (g_dev_rng && addy >= VIRTIO_RNG_BASE && addy < VIRTIO_RNG_BASE + VIRTIO_RNG_SIZE)
    return vrng_load(addy);
  else if (g_dev_9p && addy >= VIRTIO_9P_BASE && addy < VIRTIO_9P_BASE + VIRTIO_9P_SIZE)
    return v9p_load(addy);
  else if (g_dev_input && addy >= VIRTIO_INPUT_BASE && addy < VIRTIO_INPUT_BASE + VIRTIO_INPUT_SIZE)
    return vinput_load(addy);
  else if (addy >= RTC_GOLDFISH_BASE && addy < RTC_GOLDFISH_BASE + RTC_GOLDFISH_SIZE)
    return rtc_goldfish_load(addy);
  else if (addy >= PLIC_BASE && addy < PLIC_BASE + PLIC_SIZE)
    return plic_load(addy);
  return 0;
}
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value) {}
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno) { return 0; }


int main(int argc, char **argv) {
  plat_mutex_init(&ui_lock);

  if (!plat_init()) return -1;
  plat_ui_cadence(&g_top_refresh_us, &g_input_poll_us);

  term_init(&term_state);

  /* Before anything renders: the theme has to be in place for the first
     frame, or the boot log paints in the old colours and restyles itself a
     moment later. */
  cfg_load(&g_cfg);

  /* Snapshot the device flags before the emulation thread exists. The 9P tree
     toggles are finer-grained than the device - one virtio-9p channel carries
     all four trees - so they gate the trees inside v9p_init, and only turning
     off every tree removes the device. */
  g_dev_net   = g_cfg.dev_net;
  g_dev_rng   = g_cfg.dev_rng;
  g_dev_input = g_cfg.dev_sensors;
  g_dev_9p    = g_cfg.dev_sd || g_cfg.dev_nand || g_cfg.dev_twl;

  // The panel: the keyboard owns it outright, drawing into the raw
  // framebuffer. Which keyboard that is, is the backend's choice.
  pkbd_init();

  /* Pushes the loaded theme into the terminal palette and the keyboard, and
     applies font/zoom/cursor/follow. Safe this early: the emulation thread
     does not exist yet. */
  settings_apply_live();

  pkbd_draw();

  term_printf("\x1b[2J");
  term_printf("Welcome to 3DS-CLI Linux Emulator\n");
  term_printf("Initializing mini-rv32ima...\n");
  term_state.dirty = true;
  uint64_t last_present_tick = 0;
  PresentTopScreen(&last_present_tick);

  /* A diagnostic, not an optimisation: it reproduces an Old3DS-sized heap on
     a New3DS, for "Image too large for RAM" (issue #5). Capped or not, the
     walk below still backs off until malloc succeeds. */
  if (g_cfg.ram_cap_mb > 0) {
    uint32_t cap = (uint32_t)g_cfg.ram_cap_mb * 1024 * 1024;
    if (cap < ram_amt) ram_amt = cap;
  }

  while (ram_amt >= PLAT_RAM_MIN_MB * 1024u * 1024u) {
    ram_image = malloc(ram_amt);
    if (ram_image) break;
    ram_amt -= 1024 * 1024;
  }

  if (!ram_image) {
    term_printf("Failed to allocate at least %dMB for RAM.\n", PLAT_RAM_MIN_MB);
    goto wait_exit;
  }

  /* Both halves of this line are the first thing worth knowing from a bug
     report: which model's limits apply, and how much RAM the guest actually
     got - issue #5 was diagnosed off exactly these numbers. */
  term_printf("Model: %s\n", plat_model());
  term_printf("Allocated %lu bytes for RAM.\n", ram_amt);

  /* Open kernel Image (possibly a combined kernel+rootfs bundle) */
  FILE *f = fopen(PLAT_SD "Image", "rb");
  if (!f) f = fopen("Image", "rb");
  if (!f && PromptDownloadImage(&last_present_tick)) f = fopen(PLAT_SD "Image", "rb");
  if (!f) {
    term_printf("Error: Could not open '" PLAT_SD "Image'.\n");
    term_printf("Please copy Image to " PLAT_SD "\n");
    goto wait_exit;
  }

  fseek(f, 0, SEEK_END);
  long file_len = ftell(f);
  fseek(f, 0, SEEK_SET);

  uint32_t rootfs_gz_len = 0, rootfs_raw_len = 0;
  long flen = ImageKernelLen(f, file_len, &rootfs_gz_len, &rootfs_raw_len);
  bool have_bundle = flen != file_len;
  /* Gates the "Reset guest state from Image" row: with no embedded rootfs
     there is nothing to re-extract from. */
  g_have_bundle = have_bundle;

  /* A reset asked for in a previous session lands here, before anything opens
     the disk - the settings page cannot unlink rootfs.ext2 while the
     emulation thread has it open. */
  if (g_cfg.reset_rootfs) {
    g_cfg.reset_rootfs = false;
    cfg_save(&g_cfg);
    if (have_bundle) {
      remove(PLAT_SD "rootfs.ext2");
      term_printf("Resetting guest state: rootfs will be re-extracted.\n");
    } else {
      /* The row is unreachable without a bundle, but a hand-edited config
         file can still set the flag. */
      term_printf("Reset requested, but Image has no rootfs - skipped.\n");
    }
  }

  /* Open rootfs disk image for virtio-blk, extracting it from the Image
     bundle first if it isn't on the SD card yet. */
  const char *disk_path = NULL;
  FILE *disk_file = OpenDiskFile(&disk_path);
  if (!disk_file && have_bundle) {
    if (ExtractEmbeddedRootfs(f, flen, rootfs_gz_len, rootfs_raw_len, &last_present_tick))
      disk_file = OpenDiskFile(&disk_path);
  }
  if (!disk_file) {
    term_printf("Error: Could not open rootfs.ext2.\n");
    term_printf("Please copy rootfs.ext2 to " PLAT_SD "\n");
    fclose(f);
    goto wait_exit;
  }
  fseek(disk_file, 0, SEEK_END);
  uint64_t disk_size = (uint64_t)ftell(disk_file);
  fseek(disk_file, 0, SEEK_SET);
  plic_init();
  vblk_init(0, disk_file, disk_size, VIRTIO_BLK_BASE, PLIC_SRC_BLK);
  term_printf("Disk: %s (%lluMB)\n", disk_path, (unsigned long long)(disk_size >> 20));

  /* Two distinct words in the banners below: "disabled" means the user turned
     it off, "unavailable" means it was asked for and the hardware or the
     permissions weren't there. */

  /* Swap is optional: if the file can't be created (full SD, read-only
     card), instance 1 keeps base == 0, nothing answers at 0x10005000, and
     the guest's init script falls back to zram. */
  if (g_cfg.dev_swap) {
    swap_file = CreateSwapFile(&last_present_tick);
    if (swap_file) {
      vblk_init(1, swap_file, SWAP_SIZE_BYTES, VIRTIO_BLK2_BASE, PLIC_SRC_SWAP);
      term_printf("Swap: %luMB (%s)\n", (unsigned long)(SWAP_SIZE_BYTES >> 20), SWAP_PATH);
    } else {
      term_printf("Swap: unavailable (guest falls back to zram)\n");
    }
  } else {
    term_printf("Swap: disabled (guest falls back to zram)\n");
  }

  if (g_dev_rng) {
    vrng_init();
    term_printf("Hardware RNG: %s\n", vrng.ps_ready ? "ok" : "unavailable (fallback)");
  } else {
    term_printf("Hardware RNG: disabled\n");
  }

  if (g_dev_net) {
    vnet_init();
    term_printf("Network: %s\n", vnet.soc_ready ? "ok (NAT via 3DS WiFi)" : "unavailable");
  } else {
    term_printf("Network: disabled\n");
  }

  /* Passthrough + sensors. The NAND trees need extended homebrew
     permissions; without them they're simply absent and the guest's
     mount of them fails rather than the whole device disappearing. */
  if (g_dev_9p) {
    const plat_tree_t *pt;
    int npt = plat_v9p_trees(&pt);
    uint32_t want = 0;
    for (int i = 0; i < npt; i++)
      if (cfg_tree_wanted(&g_cfg, pt[i].aname)) want |= 1u << i;
    v9p_init(want);

    /* Named rather than positional: which trees a console has is its own
       business, and a bug report wants to see the ones that actually came up. */
    char list[96];
    int  ln = snprintf(list, sizeof(list), "hw");
    for (int i = 0; i < npt && ln < (int)sizeof(list); i++)
      if (v9p_tree_ok[i])
        ln += snprintf(list + ln, sizeof(list) - ln, " %s", pt[i].aname);
    term_printf("Passthrough: %s\n", list);
  } else {
    /* Every tree off takes the whole device with it, including the synthetic
       hw/ tree - there is only one virtio-9p channel. */
    term_printf("Passthrough: disabled\n");
  }

  if (g_dev_input) {
    vinput_init();
    term_printf("Sensors: %s\n", plat_caps()->sensors ? "ok (accel, gyro, sliders)"
                                                      : "unavailable");
  } else {
    term_printf("Sensors: disabled\n");
  }

  /* The RISC-V Linux Image header's image_load_offset field (8-byte LE at
     file offset 8) says how far into RAM the bootloader must place byte 0
     of this file - it's NOT always 0. MMU'd kernels (arch/riscv/kernel/
     head.S) use 0x400000 (4MB) for RV32; only the M-mode/NOMMU boot path
     this used to be uses 0. Ignoring this and always loading at RAM start
     (as this code did before real MMU support existed) puts the kernel's
     actual entry code 4MB away from where pc gets set, so the CPU starts
     executing whatever unrelated bytes happen to be at the front of the
     file instead - which decodes as an immediate illegal instruction. */
  /* The header's fields are little-endian on every host, so they are decoded
     byte by byte rather than read straight into a native integer. */
  uint64_t image_load_offset = 0, image_size = 0;
  uint8_t hdr[16];
  fseek(f, 8, SEEK_SET);
  if (fread(hdr, sizeof(hdr), 1, f) == 1) {
    for (int i = 7; i >= 0; i--) image_load_offset = (image_load_offset << 8) | hdr[i];
    for (int i = 7; i >= 0; i--) image_size        = (image_size << 8) | hdr[8 + i];
  }
  /* image_size (the next 8-byte LE field) is what the kernel will actually
     occupy once running - _end minus _start, so it counts BSS, which isn't
     in the file at all. Checking the file length instead under-counts, and
     for a kernel that squeaks past the check that means Linux silently
     zeroes its BSS over whatever follows. Fall back to the file length if
     the field is absent or nonsense (a 0 image_size is what pre-4.15
     RV32 kernels wrote). */
  if (image_size < (uint64_t)flen) image_size = (uint64_t)flen;
  fseek(f, 0, SEEK_SET);

  /* The DTB and the emulator's own CPU state sit at the very top of RAM
     (see the block below), and Linux needs a working amount of memory left
     over after the kernel image itself: page tables and the struct page
     array alone scale with the size of the RAM it's given, and everything
     from the slab to the page cache comes out of what's left. Under ~8MB
     free it either panics during boot or OOM-kills init the moment it gets
     one. */
  uint32_t kernel_top = (uint32_t)(image_load_offset + image_size);
  /* Slack for the alignment the DTB and the CPU state are rounded down to. */
  uint32_t reserved_top = sizeof(default64mbdtb) + sizeof(struct MiniRV32IMAState) + 128;
  if ((uint64_t)kernel_top + reserved_top + MIN_GUEST_FREE_RAM > (uint64_t)ram_amt) {
    term_printf("Image too large for RAM.\n");
    term_printf("  kernel wants %luMB at +%luMB, RAM is %luMB.\n",
                (unsigned long)(image_size >> 20),
                (unsigned long)(image_load_offset >> 20),
                (unsigned long)(ram_amt >> 20));
    /* Kept under 50 columns: that's all of the 80-column grid the top
       screen actually shows at the default zoom (8px glyphs, 400px wide),
       and an error nobody can read without scrolling isn't one. */
    term_printf("  Update Image and the app together: older\n"
                "  kernels needed ~29MB of guest RAM.\n");
    fclose(f); fclose(disk_file); goto wait_exit;
  }
  memset(ram_image, 0, ram_amt);
  if (fread(ram_image + image_load_offset, (size_t)flen, 1, f) != 1) {
    term_printf("Error: Could not read Image.\n");
    fclose(f); fclose(disk_file); goto wait_exit;
  }
  fclose(f);

  {
    /* Linux's fdt_check_header() requires the DTB physical address to be
       8-byte aligned (it returns -FDT_ERR_ALIGNMENT otherwise), which
       silently skips the entire device-tree scan, including memory
       detection. sizeof(default64mbdtb)/sizeof(struct MiniRV32IMAState)
       aren't multiples of 8, so this must be rounded down explicitly. */
    /* The emulator's own CPU state is a host structure that happens to live in
       the guest's allocation, so it has to satisfy the host's alignment, not
       the guest's: on a console that cares, an unaligned struct turns the
       TLB flush's memset into a fault. 64 bytes covers the widest alignment
       any of these architectures asks for. */
    uint32_t core_off = (ram_amt - sizeof(struct MiniRV32IMAState)) & ~63u;
    uint32_t dtb_ptr  = (core_off - sizeof(default64mbdtb)) & ~7u;
    memcpy(ram_image + dtb_ptr, default64mbdtb, sizeof(default64mbdtb));

    /* Patch the RAM-size word in the DTB's memory node with the actual usable
       size. dtb_ptr is the number of bytes Linux may use before the DTB.
       Device tree cells are big-endian by specification, whatever the host is,
       so the four bytes are written explicitly. */
    uint8_t *patch = ram_image + dtb_ptr + DTB_MEM_SIZE_OFFSET;
    uint32_t vr = dtb_ptr;
    patch[0] = (uint8_t)(vr >> 24);
    patch[1] = (uint8_t)(vr >> 16);
    patch[2] = (uint8_t)(vr >> 8);
    patch[3] = (uint8_t)vr;

    core = (struct MiniRV32IMAState *)(ram_image + core_off);
    core->pc = MINIRV32_RAM_IMAGE_OFFSET + (uint32_t)image_load_offset;
    core->regs[10] = 0x00; // hartid
    core->regs[11] = dtb_ptr ? (dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET) : 0; // dtb pointer

    /* Boot directly into S-mode, mirroring the register/CSR state a real
       firmware handoff (OpenSBI's `mret` into the kernel) would leave
       behind - see vendor/mini-rv32ima-mmu's file header for why there's
       no actual M-mode boot code here. medeleg delegates every exception
       the kernel needs to handle itself (including U-mode ecalls, i.e.
       syscalls); ecalls from S-mode (SBI calls, cause 9) are deliberately
       left un-delegated, so they always reach HandleException to be
       serviced. mideleg delegates the three S-level interrupt causes.
       mstatus.MIE and mie.MTIE stay permanently set so the real machine
       timer interrupt can fire at all and get forwarded to the guest as
       an STIP supervisor timer interrupt (see HandleException). */
    /* The core struct lives inside the (zeroed) RAM image, and an all-zero
       fetch cache would read as "VPN 0 is valid, at offset 0, for privilege 0
       with generation 0". Nothing the guest actually does could reach that
       state before the first satp write invalidates it, but relying on that
       is a poor trade against one store: mark it invalid outright. */
    core->fetch_tag = 0xffffffffu;

    core->extraflags |= 1; // S-mode
    core->medeleg = (1u<<0)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<12)|(1u<<13)|(1u<<15);
    core->mideleg = (1u<<1)|(1u<<5)|(1u<<9); // SSI, STI, SEI
    core->mstatus = (1u<<3); // MIE
    core->mie = (1u<<7); // MTIE
  }

  dbg_log_file = fopen(PLAT_SD "3ds-cli-debug.log", "w");

  if (!dbg_log_file) dbg_log_file = fopen("3ds-cli-debug.log", "w");
  uart_log_file = fopen(PLAT_SD "3ds-cli-console.log", "w");

  /* Before the first fprintf or fputc below - see SetStreamBuffer. */
  SetStreamBuffer(dbg_log_file, dbg_log_buf, sizeof(dbg_log_buf));
  SetStreamBuffer(uart_log_file, uart_log_buf, sizeof(uart_log_buf));

  if (dbg_log_file) {
    fprintf(dbg_log_file, "[host] vnet soc_ready=%d\n", (int)vnet.soc_ready);
    fflush(dbg_log_file);
  }

  term_printf("Booting Linux... This may take a while.\n");
  term_state.dirty = true;
  PresentTopScreen(&last_present_tick);

  // Give the emulator its own core so it can run flat-out concurrently with
  // input and rendering. Which core, and whether the console has a spare at
  // all, is the backend's call - see plat_thread_start. A single-core console
  // returns false, and the emulator is then stepped inline from the loop
  // below against the same wall-clock budget: slower, because every redraw and
  // input poll comes straight out of guest execution time, but the only shape
  // available when there is nowhere else to run.
  bool emu_thread = plat_caps()->emu_thread && plat_thread_start(EmuThreadEntry, NULL);
  uint64_t inline_tick = plat_us();
  if (emu_thread) {
    term_printf("Emulator thread: %s\n", plat_thread_desc());
  } else {
    term_printf("Emulator: inline (no spare core)\n");
  }
  term_state.dirty = true;
  PresentTopScreen(&last_present_tick);

  while (plat_running()) {


    plat_input_t in;
    plat_poll_input(&in);
    /* A real keyboard, where one is plugged in, types straight into the
       guest's ring. Only worth doing here: every other poll in this file is a
       button-driven prompt with no guest to type at yet. */
    plat_poll_keyboard();
    uint32_t kDown = in.down;
    if (kDown & PLAT_BTN_QUIT) break;

    // Sample motion/slider hardware for the virtio-input device and hand
    // it off to the emu thread. Sampling must stay on this thread: the
    // backend reads whatever the console's input poll last cached, and this
    // is the thread that polls. Piggybacking on that poll costs a handful of
    // comparisons per frame and rate-limits naturally to the refresh.
    // Skipped when the sensor device is off - nothing would read the result.
    if (g_dev_input) {
      int32_t axes[VI_NAXES];
      plat_sample_axes(axes);
      plat_mutex_lock(&ui_lock);
      memcpy(g_vinput_axes, axes, sizeof(axes));
      plat_mutex_unlock(&ui_lock);
    }

    /* Above the settings block on purpose: this is the only thing that marks
       the top screen dirty while the guest is silent, so below the goto the
       cursor freezes mid-phase for as long as the page is open.

       Skipped when blinking is off. Each edge costs a full software blit of
       the grid into the framebuffer, twice a second forever, which on an Old
       3DS comes out of guest execution time (see g_top_refresh_us). */
    if (term_state.cursor_blink) {
      static bool last_blink_on = false;
      bool blink_on = term_blink_on();
      if (blink_on != last_blink_on) {
        term_state.dirty = true;
        last_blink_on = blink_on;
      }
    }

    /* The settings page takes the whole bottom screen and every button but
       START: the keyboard's draw clears the whole panel, so the page and the
       keyboard cannot share it. */
    if (settings_open) {
      if (!settings_update(&in)) settings_leave();
      if (settings_quit) { settings_quit = false; break; }
      if (settings_open) settings_draw();
      goto after_input;
    }

    if (kDown & PLAT_BTN_SETTINGS) {
      settings_enter();
      settings_draw();
      goto after_input;
    }

    // Zoom controls (L/Y = zoom out, R/X = zoom in, always both axes equally)
    /* These shortcuts and their settings rows are the same controls, so each
       writes g_cfg as well as term_state - otherwise the page opens showing
       stale values and the next save undoes what the buttons did. */
    if (kDown & PLAT_BTN_ZOOM_OUT) {
      int z = (term_state.zoom_x > 1) ? term_state.zoom_x - 1 : 1;
      term_state.zoom_x = g_cfg.zoom_x = z;
      term_state.zoom_y = g_cfg.zoom_y = z;
      term_state.dirty = true;
    }
    if (kDown & PLAT_BTN_ZOOM_IN) {
      int z = (term_state.zoom_x < 5) ? term_state.zoom_x + 1 : 5;
      term_state.zoom_x = g_cfg.zoom_x = z;
      term_state.zoom_y = g_cfg.zoom_y = z;
      term_state.dirty = true;
    }
    if (kDown & PLAT_BTN_FOLLOW) {
      g_cfg.follow_output = !g_cfg.follow_output;
      term_state.auto_track = g_cfg.follow_output;
      if (term_state.auto_track) term_state.scroll_y = 0; // snap back to live
      term_state.dirty = true;
    }
    if (kDown & PLAT_BTN_FONT) {
      g_cfg.use_5x7 = !g_cfg.use_5x7;
      term_state.use_5x7 = g_cfg.use_5x7;
      term_state.dirty = true;
    }

    // Viewport panning with the analog stick. The deadzone is the backend's
    // (it knows its own hardware's slop), so a nonzero axis here means
    // deflected.
    static int pan_cooldown_x = 0;
    static int pan_cooldown_y = 0;

    if (g_cfg.circle_pans) {
      if (in.pan_x) {
        /* Turns follow-output off, not just auto_track: WriteUARTByte sets
           auto_track back on at the very next byte from the guest. */
        g_cfg.follow_output = false;
        term_state.auto_track = false;
        if (pan_cooldown_x <= 0) {
          if (in.pan_x > 0) term_state.scroll_x++;
          else              term_state.scroll_x--;
          pan_cooldown_x = 4;
          term_state.dirty = true;
        } else {
          pan_cooldown_x--;
        }
      } else {
        pan_cooldown_x = 0;
      }

      if (in.pan_y) {
        g_cfg.follow_output = false;
        term_state.auto_track = false;
        if (pan_cooldown_y <= 0) {
          if (in.pan_y > 0) term_state.scroll_y--;
          else              term_state.scroll_y++;
          pan_cooldown_y = 4;
          term_state.dirty = true;
        } else {
          pan_cooldown_y--;
        }
      } else {
        pan_cooldown_y = 0;
      }
    } else {
      /* Arrow-key mode: same cooldown, so a held stick repeats rather than
         flooding the ring. */
      if (in.pan_x) {
        if (pan_cooldown_x <= 0) {
          rx_push_str(in.pan_x > 0 ? "\x1b[C" : "\x1b[D");
          pan_cooldown_x = 4;
        } else pan_cooldown_x--;
      } else pan_cooldown_x = 0;

      if (in.pan_y) {
        if (pan_cooldown_y <= 0) {
          rx_push_str(in.pan_y > 0 ? "\x1b[A" : "\x1b[B");
          pan_cooldown_y = 4;
        } else pan_cooldown_y--;
      } else pan_cooldown_y = 0;
    }

    /* D-pad arrows, but only where something else can drive the panel
       keyboard. With no pointer at all (GameCube, PSP) the d-pad is how the
       keyboard's focus moves. */
    if (plat_caps()->pointer) {
      if (kDown & PLAT_BTN_UP)    rx_push_str("\x1b[A");
      if (kDown & PLAT_BTN_DOWN)  rx_push_str("\x1b[B");
      if (kDown & PLAT_BTN_RIGHT) rx_push_str("\x1b[C");
      if (kDown & PLAT_BTN_LEFT)  rx_push_str("\x1b[D");
    }

    pkbd_update(&in);
    pkbd_draw();

after_input:
    // Emulation itself now runs continuously on its own thread (see
    // EmuThreadEntry/EmuStepBatch above and the threadCreate call earlier
    // in this function) instead of being stepped from here in batches.
    // This thread's only remaining jobs are input, rendering, and noticing
    // when the emu thread reports the guest shut down or faulted.
    if (!emu_thread) {
      int ret = EmuStepBatch(&inline_tick);
      g_emu_ret = ret;
      /* WFI: the guest asked to idle, so stop burning the budget on it. */
      if (ret == 1) plat_sleep_us(1000);
    }

    int emu_ret = g_emu_ret;
    if (emu_ret == 0x5555 || emu_ret == 3 || sbi_shutdown_requested) {
      if (emu_ret == 3) term_printf("Emulator fault!\n");
      term_state.dirty = true;
      PresentTopScreen(&last_present_tick);
      break;
    }

    if (TimeSinceUs(last_present_tick, g_top_refresh_us)) {
      if (term_state.dirty) {
        PresentTopScreen(&last_present_tick);
      }
    }

    // Nothing CPU-heavy left to do on this thread each iteration now that
    // emulation runs elsewhere - yield briefly instead of spinning flat
    // out just to poll input/vblank state. ~60Hz is plenty for a touch UI
    // (coarser where the emulator shares this core - see plat_ui_cadence).
    plat_sleep_us(g_input_poll_us);
  }

  // Stop and join the emu thread before touching anything it owns
  // (ram_image, disk_file, the virtio device FILE*s, etc.) - this is the
  // one synchronization point that matters most: without it, the frees
  // and fcloses below could run concurrently with the emu thread still
  // using those same pointers/handles.
  if (emu_thread) {
    g_emu_should_stop = true;
    plat_thread_join();
  }

  /* Explicit, now that the console mirror is no longer flushed on every
     line - this is what makes a clean exit lose none of it. */
  if (uart_log_file) { fclose(uart_log_file); uart_log_file = NULL; }
  if (dbg_log_file)  { fclose(dbg_log_file);  dbg_log_file = NULL; }

  if (disk_file) fclose(disk_file);
  CloseSwapFile();
  /* v9p_exit unmounts archives and tears down hw3ds, neither of which exists
     if v9p_init never ran. */
  if (g_dev_9p) v9p_exit();
  if (vnet.soc_ready) plat_net_exit();

  /* The page saves on close too, but the button shortcuts (zoom, ZL, ZR,
     panning) change g_cfg without ever opening it. */
  cfg_save(&g_cfg);

  free(ram_image);
  plat_exit();
  return 0;

wait_exit:
  while (plat_running()) {
    plat_input_t in;
    plat_poll_input(&in);
    if (in.down & PLAT_BTN_QUIT) break;
    plat_fb_t fb;
    if (plat_surface(PLAT_SURF_TERM, &fb)) term_draw(&term_state, &fb);
    plat_present(PLAT_SURF_BIT(PLAT_SURF_TERM));
  }
  CloseSwapFile();
  free(ram_image);
  plat_exit();
  return -1;
}
