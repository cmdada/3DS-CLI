/* Nintendo Wii and GameCube backend (devkitPPC + libogc). Implements
 * source/core/plat.h. One file for both: libogc defines HW_RVL on the Wii,
 * and the differences are which capabilities exist, not how anything works.
 *
 * Neither console has a second core, so plat_thread_start always fails and
 * machine.c steps the emulator inline - see plat_caps.emu_thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>

#include <gccore.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>

#ifdef HW_RVL
#include <wiiuse/wpad.h>
#include <network.h>
#endif

#include "plat.h"

static plat_caps_t caps;
static bool        fat_ok;      /* whether libfat found a card to mount */
static char        model_str[48];

/* ---------------------------------------------------------------- video -- */

/* The video block scans a YUY2 buffer: two horizontally adjacent pixels share
   one chroma pair, so it cannot be described by plat_fb_t's strides at all.
   Everything is drawn into a plain BGRA scratch instead and converted on
   present. That costs one pass over the frame, which is why the panel and
   the terminal are both dirty-tracked and this only runs when something
   actually changed. */
static GXRModeObj *vmode;
static void       *xfb[2];
static int         xfb_next;
static uint8_t    *scratch;     /* OGC_SCREEN_W * OGC_SCREEN_H * 4, BGRA */

#define SCRATCH_STRIDE (OGC_SCREEN_W * 4)

bool plat_surface(plat_surf s, plat_fb_t *out) {
  if (!scratch) return false;
  out->x_stride = 4;
  out->y_stride = SCRATCH_STRIDE;
  out->bpp      = 4;
  if (s == PLAT_SURF_TERM) {
    out->base = scratch;
    out->w = PLAT_TERM_W; out->h = PLAT_TERM_H;
    return true;
  }
  if (s == PLAT_SURF_PANEL) {
    out->base = scratch + (size_t)PLAT_TERM_H * SCRATCH_STRIDE;
    out->w = PLAT_PANEL_W; out->h = PLAT_PANEL_H;
    return true;
  }
  return false;
}

/* BT.601, the coefficients the console's encoder expects. Two source pixels
   produce one 32-bit YUY2 word: both luma values, and the chroma of their
   average, which is what 4:2:2 subsampling means. */
static inline uint32_t ogc_yuy2(const uint8_t *p0, const uint8_t *p1) {
  int b0 = p0[0], g0 = p0[1], r0 = p0[2];
  int b1 = p1[0], g1 = p1[1], r1 = p1[2];

  int y0 = (299 * r0 + 587 * g0 + 114 * b0) / 1000;
  int y1 = (299 * r1 + 587 * g1 + 114 * b1) / 1000;
  int r  = (r0 + r1) / 2, g = (g0 + g1) / 2, b = (b0 + b1) / 2;
  int yy = (299 * r + 587 * g + 114 * b) / 1000;
  int cb = 128 + (b - yy) * 564 / 1000;
  int cr = 128 + (r - yy) * 713 / 1000;

  if (cb < 0) cb = 0; else if (cb > 255) cb = 255;
  if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
  return ((uint32_t)y0 << 24) | ((uint32_t)cb << 16) |
         ((uint32_t)y1 << 8)  | (uint32_t)cr;
}

void plat_present(unsigned mask) {
  if (!mask || !scratch) return;

  uint32_t *dst = (uint32_t *)xfb[xfb_next];
  for (int y = 0; y < OGC_SCREEN_H; y++) {
    const uint8_t *src = scratch + (size_t)y * SCRATCH_STRIDE;
    for (int x = 0; x < OGC_SCREEN_W; x += 2, src += 8)
      *dst++ = ogc_yuy2(src, src + 4);
  }

  DCFlushRange(xfb[xfb_next], OGC_SCREEN_W * OGC_SCREEN_H * 2);
  VIDEO_SetNextFramebuffer(xfb[xfb_next]);
  VIDEO_Flush();
  VIDEO_WaitVSync();
  xfb_next ^= 1;
}

/* ----------------------------------------------------------------- life -- */

bool plat_init(void) {
  VIDEO_Init();
  vmode = VIDEO_GetPreferredMode(NULL);
  xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
  xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
  if (!xfb[0] || !xfb[1]) return false;
  VIDEO_Configure(vmode);
  VIDEO_SetNextFramebuffer(xfb[0]);
  VIDEO_SetBlack(FALSE);
  VIDEO_Flush();
  VIDEO_WaitVSync();
  if (vmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

  scratch = (uint8_t *)memalign(32, (size_t)SCRATCH_STRIDE * OGC_SCREEN_H);
  if (!scratch) return false;
  memset(scratch, 0, (size_t)SCRATCH_STRIDE * OGC_SCREEN_H);

  PAD_Init();
#ifdef HW_RVL
  WPAD_Init();
  WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
  WPAD_SetVRes(WPAD_CHAN_0, OGC_SCREEN_W, OGC_SCREEN_H);
#endif

  /* libfat mounts whatever it finds; PLAT_SD names the device this console
     actually has. A failure here is the difference between "the Image is
     missing" and "there is no card at all", so it goes in the model line
     where the user will see it next to that error. */
  /* fatInitDefault reporting success only means it registered a device; what
     matters is whether the card's root can actually be read. */
  fat_ok = fatInitDefault();
  int entries = -1;
  if (fat_ok) {
    DIR *d = opendir(PLAT_SD);
    if (d) { entries = 0; while (readdir(d)) entries++; closedir(d); }
  }
  snprintf(model_str, sizeof(model_str), "%s (fat=%d " PLAT_SD "=%d)",
           PLAT_NAME, (int)fat_ok, entries);

  /* Single core on both consoles: nothing to give the emulator. */
  caps.emu_thread = false;
#ifdef HW_RVL
  caps.pointer = true;    /* the Wiimote's IR pointer */
  caps.net     = true;
  caps.sensors = true;    /* Wiimote accelerometer */
#else
  caps.pointer = false;   /* nothing can point: the keyboard uses D-pad focus */
  caps.net     = false;   /* a BBA only, and not worth assuming */
  caps.sensors = false;
#endif
  caps.rng     = false;
  caps.audio = caps.camera = caps.mic = caps.swkbd = caps.speedup = false;
  return true;
}

void plat_exit(void) {
  if (scratch) { free(scratch); scratch = NULL; }
  VIDEO_SetBlack(TRUE);
  VIDEO_Flush();
}

/* Neither console's OS asks the app to quit; START does that itself. */
bool plat_running(void) { return true; }

const plat_caps_t *plat_caps(void) { return &caps; }
const char *plat_model(void) { return model_str; }

/* The emulator shares the only core with the redraw and the input poll, so
   both run as coarsely as they can without the UI feeling dead. */
void plat_ui_cadence(uint32_t *redraw_us, uint32_t *poll_us) {
  *redraw_us = 120000;         /* ~8fps */
  *poll_us   = 1000000u / 30;
}

/* ----------------------------------------------------------------- time -- */

uint64_t plat_us(void) { return ticks_to_microsecs(gettime()); }
uint64_t plat_wallclock_ms(void) { return (uint64_t)time(NULL) * 1000ull; }
void plat_sleep_us(uint64_t us) { usleep((useconds_t)us); }

/* ---------------------------------------------------------------- input -- */

#define OGC_STICK_DEADZONE 40

static uint32_t ogc_map_pad(uint16_t b) {
  uint32_t m = 0;
  if (b & PAD_BUTTON_START) m |= PLAT_BTN_QUIT;
  if (b & PAD_TRIGGER_Z)    m |= PLAT_BTN_SETTINGS;
  if (b & PAD_TRIGGER_L)    m |= PLAT_BTN_ZOOM_OUT;
  if (b & PAD_TRIGGER_R)    m |= PLAT_BTN_ZOOM_IN;
  if (b & PAD_BUTTON_UP)    m |= PLAT_BTN_UP;
  if (b & PAD_BUTTON_DOWN)  m |= PLAT_BTN_DOWN;
  if (b & PAD_BUTTON_LEFT)  m |= PLAT_BTN_LEFT;
  if (b & PAD_BUTTON_RIGHT) m |= PLAT_BTN_RIGHT;
  if (b & PAD_BUTTON_A) m |= PLAT_BTN_A | PLAT_BTN_SWKBD;
  if (b & PAD_BUTTON_B) m |= PLAT_BTN_B;
  if (b & PAD_BUTTON_X) m |= PLAT_BTN_X | PLAT_BTN_ZOOM_IN;
  if (b & PAD_BUTTON_Y) m |= PLAT_BTN_Y | PLAT_BTN_ZOOM_OUT;
  return m;
}

#ifdef HW_RVL
static uint32_t ogc_map_wpad(uint32_t b) {
  uint32_t m = 0;
  if (b & WPAD_BUTTON_HOME)  m |= PLAT_BTN_QUIT;
  if (b & WPAD_BUTTON_MINUS) m |= PLAT_BTN_SETTINGS;
  if (b & WPAD_BUTTON_PLUS)  m |= PLAT_BTN_ZOOM_IN;
  if (b & WPAD_BUTTON_1)     m |= PLAT_BTN_ZOOM_OUT;
  if (b & WPAD_BUTTON_2)     m |= PLAT_BTN_FONT;
  /* The Wiimote is held sideways, so its d-pad is rotated a quarter turn. */
  if (b & WPAD_BUTTON_RIGHT) m |= PLAT_BTN_UP;
  if (b & WPAD_BUTTON_LEFT)  m |= PLAT_BTN_DOWN;
  if (b & WPAD_BUTTON_UP)    m |= PLAT_BTN_LEFT;
  if (b & WPAD_BUTTON_DOWN)  m |= PLAT_BTN_RIGHT;
  if (b & WPAD_BUTTON_A) m |= PLAT_BTN_A | PLAT_BTN_SWKBD;
  if (b & WPAD_BUTTON_B) m |= PLAT_BTN_B;
  return m;
}
#endif

void plat_poll_input(plat_input_t *out) {
  memset(out, 0, sizeof(*out));

  PAD_ScanPads();
  out->down = ogc_map_pad(PAD_ButtonsDown(0));
  out->held = ogc_map_pad(PAD_ButtonsHeld(0));

  int sx = PAD_StickX(0), sy = PAD_StickY(0);
  out->pan_x = (sx >  OGC_STICK_DEADZONE) ?  1 : (sx < -OGC_STICK_DEADZONE) ? -1 : 0;
  out->pan_y = (sy >  OGC_STICK_DEADZONE) ? -1 : (sy < -OGC_STICK_DEADZONE) ?  1 : 0;

#ifdef HW_RVL
  WPAD_ScanPads();
  out->down |= ogc_map_wpad(WPAD_ButtonsDown(0));
  out->held |= ogc_map_wpad(WPAD_ButtonsHeld(0));

  /* The IR pointer is the panel's pointer. Off-sensor-bar reads come back
     invalid, which is exactly when the keyboard should ignore it. */
  ir_t ir;
  WPAD_IR(WPAD_CHAN_0, &ir);
  int px = (int)ir.x, py = (int)ir.y - PLAT_TERM_H;
  bool on_panel = ir.valid && px >= 0 && px < PLAT_PANEL_W &&
                              py >= 0 && py < PLAT_PANEL_H;
  static bool was_down = false;
  bool pressed = on_panel && (WPAD_ButtonsHeld(0) & WPAD_BUTTON_A);
  out->ptr_valid  = on_panel;
  out->ptr_down   = pressed;
  out->ptr_tapped = pressed && !was_down;
  out->ptr_x = (int16_t)px;
  out->ptr_y = (int16_t)py;
  was_down = pressed;
#endif
}

/* -------------------------------------------------------------- threads -- */

bool plat_thread_start(void (*entry)(void *), void *arg) {
  (void)entry; (void)arg;
  return false;   /* single core; machine.c steps the emulator inline */
}

void plat_thread_join(void) {}
const char *plat_thread_desc(void) { return "inline"; }

void plat_mutex_init(plat_mutex_t *m)   { LWP_MutexInit((mutex_t *)m, false); }
void plat_mutex_lock(plat_mutex_t *m)   { LWP_MutexLock(*(mutex_t *)m); }
void plat_mutex_unlock(plat_mutex_t *m) { LWP_MutexUnlock(*(mutex_t *)m); }

/* ------------------------------------------------------------------ net -- */

bool plat_net_init(void) {
#ifdef HW_RVL
  char ip[16];
  return if_config(ip, NULL, NULL, true, 20) >= 0;
#else
  return false;   /* a Broadband Adapter only */
#endif
}

void plat_net_exit(void) {}

/* -------------------------------------------------------------- entropy -- */

bool plat_random(void *buf, size_t len) { (void)buf; (void)len; return false; }

/* -------------------------------------------------------------- sensors -- */

void plat_sample_axes(int32_t *out) {
  memset(out, 0, sizeof(int32_t) * VI_NAXES);
#ifdef HW_RVL
  WPADData *d = WPAD_Data(0);
  if (d) {
    out[0] = (int32_t)d->accel.x - 512;
    out[1] = (int32_t)d->accel.y - 512;
    out[2] = (int32_t)d->accel.z - 512;
  }
#endif
}

/* ------------------------------------------------------------- 9P trees -- */

static const plat_tree_t trees[] = {
  { "sd", PLAT_SD, false },
};

int plat_v9p_trees(const plat_tree_t **out) {
  *out = trees;
  return (int)(sizeof(trees) / sizeof(trees[0]));
}

bool plat_v9p_mount(int idx) { return idx == 0 && fat_ok; }
void plat_v9p_unmount_all(void) {}
int64_t plat_v9p_free_bytes(void) { return -1; }

/* -------------------------------------------------------------- hw tree -- */

static int hw_rd_console_size(char *b, int n) {
  return snprintf(b, n, "%d %d\n", TERM_COLS, TERM_ROWS);
}
static int hw_rd_model(char *b, int n) { return snprintf(b, n, "%s\n", PLAT_NAME); }

#ifdef HW_RVL
static int hw_rd_accel(char *b, int n) {
  WPADData *d = WPAD_Data(0);
  if (!d) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%d %d %d\n",
                  (int)d->accel.x - 512, (int)d->accel.y - 512, (int)d->accel.z - 512);
}
static int hw_rd_battery(char *b, int n) {
  WPADData *d = WPAD_Data(0);
  if (!d) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%u\n", (unsigned)d->battery_level);
}
#endif

static int hw_rd_info(char *b, int n) {
  return snprintf(b, n, "model:    %s\nscreen:   %dx%d\nconsole:  %dx%d\n",
                  PLAT_NAME, OGC_SCREEN_W, OGC_SCREEN_H, TERM_COLS, TERM_ROWS);
}

const plat_hw_ent plat_hw_files[] = {
  { "info",         0444, hw_rd_info,         NULL, HWS_NONE },
  { "model",        0444, hw_rd_model,        NULL, HWS_NONE },
#ifdef HW_RVL
  { "accel",        0444, hw_rd_accel,        NULL, HWS_NONE },
  { "battery",      0444, hw_rd_battery,      NULL, HWS_NONE },
#endif
  { "console_size", 0444, hw_rd_console_size, NULL, HWS_NONE },
};
const int plat_hw_count = (int)(sizeof(plat_hw_files) / sizeof(plat_hw_files[0]));

int plat_hw_camera(bool inner, uint8_t **frame) { (void)inner; (void)frame; return 0; }
int plat_hw_mic_read(uint8_t *out, int max)     { (void)out; (void)max; return 0; }
int plat_hw_audio_write(const uint8_t *d, int l) { (void)d; return l; }
