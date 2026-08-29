/* Nintendo Wii U backend (devkitPPC + wut). Implements source/core/plat.h.
 *
 * The TV is the guest's console and the GamePad is the panel, which is the
 * same shape as the 3DS's two screens - the GamePad is a touch panel, so the
 * keyboard and the settings page work exactly as they do there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/mutex.h>
#include <nn/ac.h>
#include <nsyskbd/nsyskbd.h>
#include <vpad/input.h>
#include <whb/proc.h>
#include <whb/sdcard.h>

#include "plat.h"
#include "hid_ascii.h"

_Static_assert(sizeof(OSMutex) <= PLAT_MUTEX_SIZE, "PLAT_MUTEX_SIZE too small for OSMutex");

static plat_caps_t caps;

/* ---------------------------------------------------------------- video -- */

/* The DRC is 854x480; the panel is a logical 320x240 doubled into the middle
   of it (see plat_cfg.h), which leaves this margin either side. */
#define DRC_W        854
#define DRC_H        480
#define PANEL_OFF_X  ((DRC_W - PLAT_PANEL_W * CAFE_PANEL_SCALE) / 2)
#define PANEL_OFF_Y  ((DRC_H - PLAT_PANEL_H * CAFE_PANEL_SCALE) / 2)

typedef struct {
  OSScreenID id;
  uint8_t   *mem;
  uint32_t   size;
  int        w, h;
  ptrdiff_t  xs, ys;
  uint8_t   *origin[2];   /* work-buffer origin, indexed by flip parity */
  int        parity;
  bool       probed;      /* false: the fallback layout below is in use */
} cafe_screen;

static cafe_screen scr_tv, scr_drc;
static uint8_t    *panel_buf;   /* linear 320x240 the panel is drawn into */

/* OSScreen's in-memory layout is undocumented, differs between the two screens
   and is not something to hardcode, so it is measured: clear a window at the
   start of the area, ask OSScreen to plot exactly one pixel, and find which
   word changed. Three plots give the origin and both strides - precisely what
   plat_fb_t wants - and the answer is right on hardware and under an emulator
   alike.

   The window is deliberately small. Clearing and scanning the whole area (some
   7MB for the TV, four times over) is enough memory traffic to take an
   emulator down, and every offset being looked for is near the start anyway:
   the origin at 0, the x step one pixel along, the y step one row. */
#define CAFE_PROBE_WINDOW (64u * 1024u)

static size_t cafe_probe(cafe_screen *s, uint32_t x, uint32_t y, size_t limit) {
  if (limit > s->size) limit = s->size;
  memset(s->mem, 0, limit);
  OSScreenPutPixelEx(s->id, x, y, 0xffffff00);
  /* No cache maintenance here on purpose. Both the clear and the plot are CPU
     writes and this is a CPU read, so they are coherent through the cache -
     and invalidating would discard the very pixel being looked for. */
  const uint32_t *w = (const uint32_t *)s->mem;
  for (size_t i = 0; i < limit / 4; i++)
    if (w[i]) return i * 4;
  return (size_t)-1;
}

static bool cafe_screen_init(cafe_screen *s, OSScreenID id, int w, int h) {
  s->id = id; s->w = w; s->h = h;
  s->size = OSScreenGetBufferSizeEx(id);
  s->mem  = (uint8_t *)MEMAllocFromDefaultHeapEx(s->size, 0x100);
  if (!s->mem) return false;
  OSScreenSetBufferEx(id, s->mem);
  OSScreenEnableEx(id, TRUE);

  size_t o00 = cafe_probe(s, 0, 0, CAFE_PROBE_WINDOW);
  size_t o10 = cafe_probe(s, 1, 0, CAFE_PROBE_WINDOW);
  size_t o01 = cafe_probe(s, 0, 1, CAFE_PROBE_WINDOW);

  if (o00 == (size_t)-1 || o10 == (size_t)-1 || o01 == (size_t)-1) {
    /* Nothing was found where OSScreen was asked to plot. Fall back to a plain
       linear layout and both buffers back to back, which is what the hardware
       actually does - a wrong guess draws a scrambled screen, which is still
       something the user can report, where giving up draws nothing at all. */
    s->xs = 4;
    s->ys = (ptrdiff_t)w * 4;
    s->origin[0] = s->mem;
    s->origin[1] = s->mem + s->size / 2;
    s->probed = false;
  } else {
    s->xs = (ptrdiff_t)o10 - (ptrdiff_t)o00;
    s->ys = (ptrdiff_t)o01 - (ptrdiff_t)o00;
    s->origin[0] = s->mem + o00;

    /* Both buffers live in the one area, the second right after the first:
       OSScreenGetBufferSizeEx returns room for the pair, and the measured row
       stride accounts for exactly half of it. */
    s->origin[1] = s->mem + s->size / 2;
    s->probed = true;
  }
  s->parity = 0;

  OSScreenClearBufferEx(id, 0);
  OSScreenFlipBuffersEx(id);
  OSScreenClearBufferEx(id, 0);
  return true;
}

static void cafe_fb(const cafe_screen *s, plat_fb_t *out) {
  out->base     = s->origin[s->parity];
  out->w        = s->w;
  out->h        = s->h;
  out->x_stride = (int)s->xs;
  out->y_stride = (int)s->ys;
  out->bpp      = 4;
}

/* Flush only the span the work buffer actually occupies. The whole area is
   several megabytes and both buffers are in it; flushing all of it every
   frame would cost more than the drawing did. */
static void cafe_flush(const cafe_screen *s) {
  ptrdiff_t ex = (ptrdiff_t)(s->w - 1) * s->xs;
  ptrdiff_t ey = (ptrdiff_t)(s->h - 1) * s->ys;
  uint8_t *o  = s->origin[s->parity];
  uint8_t *lo = o + (ex < 0 ? ex : 0) + (ey < 0 ? ey : 0);
  uint8_t *hi = o + (ex > 0 ? ex : 0) + (ey > 0 ? ey : 0) + 4;
  DCFlushRange(lo, (uint32_t)(hi - lo));
}

bool plat_surface(plat_surf s, plat_fb_t *out) {
  if (s == PLAT_SURF_TERM) { cafe_fb(&scr_tv, out); return true; }
  if (s == PLAT_SURF_PANEL) {
    out->base     = panel_buf;
    out->w        = PLAT_PANEL_W;
    out->h        = PLAT_PANEL_H;
    out->x_stride = 4;
    out->y_stride = PLAT_PANEL_W * 4;
    out->bpp      = 4;
    return true;
  }
  return false;
}

/* The panel is drawn at 320x240 and doubled on the way out, so the layout
   constants the keyboard and settings page were written against still land on
   whole pixels. */
static void cafe_present_panel(void) {
  const cafe_screen *s = &scr_drc;
  uint8_t *dst0 = s->origin[s->parity];
  for (int y = 0; y < PLAT_PANEL_H; y++) {
    const uint8_t *src = panel_buf + (size_t)y * PLAT_PANEL_W * 4;
    for (int x = 0; x < PLAT_PANEL_W; x++, src += 4) {
      int dx = PANEL_OFF_X + x * CAFE_PANEL_SCALE;
      int dy = PANEL_OFF_Y + y * CAFE_PANEL_SCALE;
      for (int ry = 0; ry < CAFE_PANEL_SCALE; ry++) {
        uint8_t *d = dst0 + (ptrdiff_t)dx * s->xs + (ptrdiff_t)(dy + ry) * s->ys;
        for (int rx = 0; rx < CAFE_PANEL_SCALE; rx++, d += s->xs) {
          d[0] = src[0]; d[1] = src[1]; d[2] = src[2];
        }
      }
    }
  }
}

void plat_present(unsigned mask) {
  if (mask & PLAT_SURF_BIT(PLAT_SURF_TERM)) {
    cafe_flush(&scr_tv);
    OSScreenFlipBuffersEx(SCREEN_TV);
    scr_tv.parity ^= 1;
  }
  if (mask & PLAT_SURF_BIT(PLAT_SURF_PANEL)) {
    cafe_present_panel();
    cafe_flush(&scr_drc);
    OSScreenFlipBuffersEx(SCREEN_DRC);
    scr_drc.parity ^= 1;
  }
}

/* ----------------------------------------------------------------- life -- */

/* Defined with the rest of the keyboard, below; KBDSetup wants them here. */
static void cafe_kbd_attach(KBDAttachEvent *e);
static void cafe_kbd_detach(KBDAttachEvent *e);
static void cafe_kbd_key(KBDKeyEvent *e);

bool plat_init(void) {
  WHBProcInit();
  /* Nothing else mounts it: WHBProcInit brings up the process, not the
     filesystem, and every path the app touches lives on the card. */
  WHBMountSdCard();
  OSScreenInit();

  panel_buf = (uint8_t *)MEMAllocFromDefaultHeapEx(PLAT_PANEL_W * PLAT_PANEL_H * 4, 64);
  if (!panel_buf) return false;
  memset(panel_buf, 0, PLAT_PANEL_W * PLAT_PANEL_H * 4);

  if (!cafe_screen_init(&scr_tv, SCREEN_TV, PLAT_TERM_W, PLAT_TERM_H)) return false;
  {
    char m[128];
    snprintf(m, sizeof(m), "tv: size=%u xs=%d ys=%d o0=%d o1=%d probed=%d",
             (unsigned)scr_tv.size, (int)scr_tv.xs, (int)scr_tv.ys,
             (int)(scr_tv.origin[0] - scr_tv.mem), (int)(scr_tv.origin[1] - scr_tv.mem),
             (int)scr_tv.probed);
  }
  if (!cafe_screen_init(&scr_drc, SCREEN_DRC, DRC_W, DRC_H)) return false;
  {
    char m[128];
    snprintf(m, sizeof(m), "drc: size=%u xs=%d ys=%d probed=%d",
             (unsigned)scr_drc.size, (int)scr_drc.xs, (int)scr_drc.ys, (int)scr_drc.probed);
  }

  VPADInit();
  /* A USB keyboard in either of the console's ports. The attach callback is
     what sets caps.keyboard, so nothing is assumed about one being there. */
  KBDSetup(cafe_kbd_attach, cafe_kbd_detach, cafe_kbd_key);

  caps.emu_thread = true;
  caps.pointer    = true;
  caps.net        = true;
  caps.sensors    = true;
  caps.rng        = false;   /* no CSPRNG service; virtio_rng falls back */
  caps.audio      = false;
  caps.camera     = false;
  caps.mic        = false;
  caps.swkbd      = false;   /* nn::swkbd is a separate job - see plat_kbd.h */
  caps.speedup    = false;
  return true;
}

void plat_exit(void) {
  KBDTeardown();
  if (caps.net) ACFinalize();
  OSScreenShutdown();
  WHBUnmountSdCard();
  WHBProcShutdown();
}

bool plat_running(void) { return WHBProcIsRunning(); }

const plat_caps_t *plat_caps(void) { return &caps; }
const char *plat_model(void) {
  return scr_tv.probed ? "Wii U (TV + GamePad)"
                       : "Wii U (TV + GamePad, assumed screen layout)";
}

/* Three cores and the emulator has one to itself, so neither the redraw nor
   the input poll competes with guest execution. */
void plat_ui_cadence(uint32_t *redraw_us, uint32_t *poll_us) {
  *redraw_us = 33000;          /* ~30fps */
  *poll_us   = 1000000u / 60;
}

/* ----------------------------------------------------------------- time -- */

uint64_t plat_us(void) { return OSTicksToMicroseconds(OSGetSystemTime()); }
uint64_t plat_wallclock_ms(void) { return (uint64_t)time(NULL) * 1000ull; }
void plat_sleep_us(uint64_t us) { OSSleepTicks(OSMicrosecondsToTicks(us)); }

/* ---------------------------------------------------------------- input -- */

/* GamePad stick units are floats in [-1, 1]. Below this the stick is treated
   as centred; it is the hardware's slop, not a UI preference. */
#define CAFE_STICK_DEADZONE 0.3f

static uint32_t cafe_map(uint32_t b) {
  uint32_t m = 0;
  if (b & VPAD_BUTTON_PLUS)  m |= PLAT_BTN_QUIT;
  if (b & VPAD_BUTTON_MINUS) m |= PLAT_BTN_SETTINGS;
  if (b & (VPAD_BUTTON_L | VPAD_BUTTON_Y)) m |= PLAT_BTN_ZOOM_OUT;
  if (b & (VPAD_BUTTON_R | VPAD_BUTTON_X)) m |= PLAT_BTN_ZOOM_IN;
  if (b & VPAD_BUTTON_ZL) m |= PLAT_BTN_FOLLOW;
  if (b & VPAD_BUTTON_ZR) m |= PLAT_BTN_FONT;
  if (b & VPAD_BUTTON_UP)    m |= PLAT_BTN_UP;
  if (b & VPAD_BUTTON_DOWN)  m |= PLAT_BTN_DOWN;
  if (b & VPAD_BUTTON_LEFT)  m |= PLAT_BTN_LEFT;
  if (b & VPAD_BUTTON_RIGHT) m |= PLAT_BTN_RIGHT;
  if (b & VPAD_BUTTON_A) m |= PLAT_BTN_A | PLAT_BTN_SWKBD;
  if (b & VPAD_BUTTON_B) m |= PLAT_BTN_B;
  if (b & VPAD_BUTTON_X) m |= PLAT_BTN_X;
  if (b & VPAD_BUTTON_Y) m |= PLAT_BTN_Y;
  return m;
}

static VPADStatus vpad;
static bool       vpad_ok;

void plat_poll_input(plat_input_t *out) {
  VPADReadError err;
  vpad_ok = VPADRead(VPAD_CHAN_0, &vpad, 1, &err) > 0 && err == VPAD_READ_SUCCESS;
  if (!vpad_ok) { memset(out, 0, sizeof(*out)); return; }

  out->down = cafe_map(vpad.trigger);
  out->held = cafe_map(vpad.hold);

  out->pan_x = (vpad.leftStick.x >  CAFE_STICK_DEADZONE) ?  1
             : (vpad.leftStick.x < -CAFE_STICK_DEADZONE) ? -1 : 0;
  /* Screen y grows downward, the stick's does not. */
  out->pan_y = (vpad.leftStick.y >  CAFE_STICK_DEADZONE) ? -1
             : (vpad.leftStick.y < -CAFE_STICK_DEADZONE) ?  1 : 0;

  /* tpNormal is raw; the calibrated point is in DRC screen space, which is
     what maps onto the doubled panel. */
  VPADTouchData tp;
  VPADGetTPCalibratedPoint(VPAD_CHAN_0, &tp, &vpad.tpFiltered1);

  int px = ((int)tp.x - PANEL_OFF_X) / CAFE_PANEL_SCALE;
  int py = ((int)tp.y - PANEL_OFF_Y) / CAFE_PANEL_SCALE;
  bool on_panel = tp.touched && px >= 0 && px < PLAT_PANEL_W &&
                                py >= 0 && py < PLAT_PANEL_H;

  static bool was_down = false;
  out->ptr_valid  = on_panel;
  out->ptr_down   = on_panel;
  out->ptr_tapped = on_panel && !was_down;
  out->ptr_x = (int16_t)px;
  out->ptr_y = (int16_t)py;
  was_down = on_panel;
}

/* ------------------------------------------------------------- keyboard -- */

/* nsyskbd is the only keyboard interface here that pushes rather than polls,
   and its callbacks run on the driver's own thread. So they translate into a
   byte ring and plat_poll_keyboard drains it, which keeps every rx_push on
   the thread core expects.
 *
 * Single producer, single consumer, and a full ring drops rather than blocks:
 * losing a keystroke is better than stalling the input driver. */
#define CAFE_KBD_RING 64
static volatile char     kbd_ring[CAFE_KBD_RING];
static volatile uint32_t kbd_head, kbd_tail;
static volatile bool     kbd_ctrl, kbd_shift;

static void cafe_kbd_put(const char *s) {
  for (; *s; s++) {
    uint32_t next = (kbd_head + 1) % CAFE_KBD_RING;
    if (next == kbd_tail) return;
    kbd_ring[kbd_head] = *s;
    kbd_head = next;
  }
}

static void cafe_kbd_attach(KBDAttachEvent *e) { (void)e; caps.keyboard = true; }
static void cafe_kbd_detach(KBDAttachEvent *e) { (void)e; caps.keyboard = false; }

static void cafe_kbd_key(KBDKeyEvent *e) {
  uint8_t usage = e->hidCode;

  /* The modifiers arrive as ordinary key events, and nothing else reports
     their state, so they are tracked here. */
  if (usage == 0xE0 || usage == 0xE4) { kbd_ctrl  = e->isPressedDown; return; }
  if (usage == 0xE1 || usage == 0xE5) { kbd_shift = e->isPressedDown; return; }
  if (!e->isPressedDown) return;

  char buf[8];
  uint16_t ch = e->asUTF16Character;
  /* The driver has already applied the user's own layout, so its character is
     better than anything the usage id could be turned into - but only for
     printable ones. Enter, tab and backspace come back as control codes that
     disagree with what the other backends send, so those go through the
     shared table instead and stay consistent. */
  if (!kbd_ctrl && ch >= 0x20 && ch < 0x7f) {
    buf[0] = (char)ch; buf[1] = 0;
    cafe_kbd_put(buf);
    return;
  }
  const char *out = hid_term_bytes(usage, kbd_shift, kbd_ctrl, false, buf);
  if (out) cafe_kbd_put(out);
}

void plat_poll_keyboard(void) {
  while (kbd_tail != kbd_head) {
    rx_push(kbd_ring[kbd_tail]);
    kbd_tail = (kbd_tail + 1) % CAFE_KBD_RING;
  }
}

/* -------------------------------------------------------------- threads -- */

/* CPU1 is where WHBProc runs the app, so the emulator gets CPU2 to itself. */
#define EMU_STACK_SIZE (128 * 1024)
static OSThread  emu_thread;
static uint8_t  *emu_stack;
static bool      emu_started;

static int emu_trampoline(int argc, const char **argv) {
  void (*entry)(void *) = (void (*)(void *))argv;
  (void)argc;
  entry(NULL);
  return 0;
}

bool plat_thread_start(void (*entry)(void *), void *arg) {
  (void)arg;
  emu_stack = (uint8_t *)MEMAllocFromDefaultHeapEx(EMU_STACK_SIZE, 16);
  if (!emu_stack) return false;
  if (!OSCreateThread(&emu_thread, emu_trampoline, 0, (char *)entry,
                      emu_stack + EMU_STACK_SIZE, EMU_STACK_SIZE, 16,
                      OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
    return false;
  }
  OSResumeThread(&emu_thread);
  emu_started = true;
  return true;
}

void plat_thread_join(void) {
  if (!emu_started) return;
  int result = 0;
  OSJoinThread(&emu_thread, &result);
  emu_started = false;
}

const char *plat_thread_desc(void) { return "CPU2"; }

void plat_mutex_init(plat_mutex_t *m)   { OSInitMutex((OSMutex *)m); }
void plat_mutex_lock(plat_mutex_t *m)   { OSLockMutex((OSMutex *)m); }
void plat_mutex_unlock(plat_mutex_t *m) { OSUnlockMutex((OSMutex *)m); }

/* ------------------------------------------------------------------ net -- */

bool plat_net_init(void) {
  if (!NNResult_IsSuccess(ACInitialize())) return false;
  if (!NNResult_IsSuccess(ACConnect())) { ACFinalize(); return false; }
  return true;
}

void plat_net_exit(void) { ACFinalize(); }

/* -------------------------------------------------------------- entropy -- */

bool plat_random(void *buf, size_t len) { (void)buf; (void)len; return false; }

/* -------------------------------------------------------------- sensors -- */

/* Accel is in g and gyro in degrees; scaled to the integer ranges
   virtio_input.h advertises so the guest sees the same axis shape it does on
   a 3DS. The last two axes are sliders the GamePad does not have. */
void plat_sample_axes(int32_t *out) {
  if (!vpad_ok) { memset(out, 0, sizeof(int32_t) * VI_NAXES); return; }
  out[0] = (int32_t)(vpad.accelorometer.acc.x * 512.0f);
  out[1] = (int32_t)(vpad.accelorometer.acc.y * 512.0f);
  out[2] = (int32_t)(vpad.accelorometer.acc.z * 512.0f);
  out[3] = (int32_t)(vpad.gyro.x * 100.0f);
  out[4] = (int32_t)(vpad.gyro.y * 100.0f);
  out[5] = (int32_t)(vpad.gyro.z * 100.0f);
  out[6] = 0;
  out[7] = 0;
}

/* ------------------------------------------------------------- 9P trees -- */

static const plat_tree_t trees[] = {
  { "sd", PLAT_SD, false },
};

int plat_v9p_trees(const plat_tree_t **out) {
  *out = trees;
  return (int)(sizeof(trees) / sizeof(trees[0]));
}

/* Already mounted by the loader; nothing to do but confirm it is reachable. */
bool plat_v9p_mount(int idx) {
  if (idx != 0) return false;
  FILE *f = fopen(PLAT_SD "3ds-cli.cfg", "ab");
  if (f) fclose(f);
  return true;
}

void plat_v9p_unmount_all(void) {}

int64_t plat_v9p_free_bytes(void) { return -1; }

/* -------------------------------------------------------------- hw tree -- */

static int hw_rd_console_size(char *b, int n) {
  return snprintf(b, n, "%d %d\n", TERM_COLS, TERM_ROWS);
}
static int hw_rd_model(char *b, int n) { return snprintf(b, n, "Wii U\n"); }

static int hw_rd_accel(char *b, int n) {
  if (!vpad_ok) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%d %d %d\n",
                  (int)(vpad.accelorometer.acc.x * 512.0f),
                  (int)(vpad.accelorometer.acc.y * 512.0f),
                  (int)(vpad.accelorometer.acc.z * 512.0f));
}
static int hw_rd_gyro(char *b, int n) {
  if (!vpad_ok) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%d %d %d\n",
                  (int)(vpad.gyro.x * 100.0f),
                  (int)(vpad.gyro.y * 100.0f),
                  (int)(vpad.gyro.z * 100.0f));
}
/* Absolute orientation, which the 3DS has no equivalent of. */
static int hw_rd_angle(char *b, int n) {
  if (!vpad_ok) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%d %d %d\n",
                  (int)(vpad.angle.x * 100.0f),
                  (int)(vpad.angle.y * 100.0f),
                  (int)(vpad.angle.z * 100.0f));
}
static int hw_rd_info(char *b, int n) {
  return snprintf(b, n, "model:    Wii U\nscreen:   %dx%d TV, %dx%d GamePad\nconsole:  %dx%d\n",
                  PLAT_TERM_W, PLAT_TERM_H, DRC_W, DRC_H, TERM_COLS, TERM_ROWS);
}

const plat_hw_ent plat_hw_files[] = {
  { "info",         0444, hw_rd_info,         NULL, HWS_NONE },
  { "model",        0444, hw_rd_model,        NULL, HWS_NONE },
  { "accel",        0444, hw_rd_accel,        NULL, HWS_NONE },
  { "gyro",         0444, hw_rd_gyro,         NULL, HWS_NONE },
  { "angle",        0444, hw_rd_angle,        NULL, HWS_NONE },
  { "console_size", 0444, hw_rd_console_size, NULL, HWS_NONE },
};
const int plat_hw_count = (int)(sizeof(plat_hw_files) / sizeof(plat_hw_files[0]));

int plat_hw_camera(bool inner, uint8_t **frame) { (void)inner; (void)frame; return 0; }
int plat_hw_mic_read(uint8_t *out, int max)     { (void)out; (void)max; return 0; }
int plat_hw_audio_write(const uint8_t *d, int l) { (void)d; return l; }
