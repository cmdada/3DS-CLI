/* Nintendo Switch backend (devkitA64 + libnx). Implements source/core/plat.h.
 *
 * One screen, split: the guest's console fills the top 480 rows and the panel
 * sits in the bottom 240, so the keyboard and the settings page are always
 * visible exactly as they are on a console with two screens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <switch.h>

#include "plat.h"
#include "hid_ascii.h"

_Static_assert(sizeof(Mutex) <= PLAT_MUTEX_SIZE, "PLAT_MUTEX_SIZE too small for Mutex");

static plat_caps_t caps;

/* ---------------------------------------------------------------- video -- */

#define PANEL_OFF_X ((NX_SCREEN_W - PLAT_PANEL_W) / 2)
#define PANEL_OFF_Y PLAT_TERM_H

static Framebuffer nx_fb;
static uint8_t    *nx_base;
static uint32_t    nx_stride;

bool plat_surface(plat_surf s, plat_fb_t *out) {
  if (!nx_base) return false;
  out->x_stride = 4;
  out->y_stride = (int)nx_stride;
  out->bpp      = 4;
  if (s == PLAT_SURF_TERM) {
    out->base = nx_base;
    out->w    = PLAT_TERM_W;
    out->h    = PLAT_TERM_H;
    return true;
  }
  if (s == PLAT_SURF_PANEL) {
    out->base = nx_base + (size_t)PANEL_OFF_Y * nx_stride + (size_t)PANEL_OFF_X * 4;
    out->w    = PLAT_PANEL_W;
    out->h    = PLAT_PANEL_H;
    return true;
  }
  return false;
}

/* Both surfaces are sub-rectangles of one framebuffer, so either one changing
   means the whole thing is presented. */
void plat_present(unsigned mask) {
  if (!mask || !nx_base) return;
  framebufferEnd(&nx_fb);
  nx_base = (uint8_t *)framebufferBegin(&nx_fb, &nx_stride);
}

/* ----------------------------------------------------------------- life -- */

static PadState pad;

bool plat_init(void) {
  /* A linear shadow buffer, so the surfaces above are plain strides rather
     than the swizzled layout the display block actually wants; libnx converts
     on framebufferEnd. */
  framebufferCreate(&nx_fb, nwindowGetDefault(), NX_SCREEN_W, NX_SCREEN_H,
                    PIXEL_FORMAT_RGBA_8888, 2);
  framebufferMakeLinear(&nx_fb);
  nx_base = (uint8_t *)framebufferBegin(&nx_fb, &nx_stride);
  if (!nx_base) return false;
  memset(nx_base, 0, (size_t)nx_stride * NX_SCREEN_H);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  hidInitializeTouchScreen();
  /* A USB keyboard on the dock, or one paired over Bluetooth; hid reports
     both through the same interface. Whether anything is actually there is
     asked every frame in plat_poll_keyboard. */
  hidInitializeKeyboard();

  caps.emu_thread = true;
  caps.pointer    = true;
  caps.net        = true;
  caps.rng        = true;    /* randomGet is always available */
  caps.sensors    = false;   /* six-axis needs a bound controller; see below */
  caps.audio      = false;
  caps.camera     = false;
  caps.mic        = false;
  caps.swkbd      = true;
  caps.speedup    = false;
  return true;
}

void plat_exit(void) {
  if (caps.net) socketExit();
  framebufferClose(&nx_fb);
}

bool plat_running(void) { return appletMainLoop(); }

const plat_caps_t *plat_caps(void) { return &caps; }

const char *plat_model(void) {
  return appletGetOperationMode() == AppletOperationMode_Console
           ? "Switch (docked)" : "Switch (handheld)";
}

/* Four cores, three of them the application's, so the emulator has one to
   itself and neither the redraw nor the input poll costs guest time. */
void plat_ui_cadence(uint32_t *redraw_us, uint32_t *poll_us) {
  *redraw_us = 16666;          /* 60fps */
  *poll_us   = 1000000u / 60;
}

/* ----------------------------------------------------------------- time -- */

uint64_t plat_us(void) { return armTicksToNs(armGetSystemTick()) / 1000ull; }
uint64_t plat_wallclock_ms(void) { return (uint64_t)time(NULL) * 1000ull; }
void plat_sleep_us(uint64_t us) { svcSleepThread((int64_t)us * 1000); }

/* ---------------------------------------------------------------- input -- */

/* Stick range is +-32767; below this the stick counts as centred. */
#define NX_STICK_DEADZONE 12000

static uint32_t nx_map(uint64_t b) {
  uint32_t m = 0;
  if (b & HidNpadButton_Plus)  m |= PLAT_BTN_QUIT;
  if (b & HidNpadButton_Minus) m |= PLAT_BTN_SETTINGS;
  if (b & (HidNpadButton_L | HidNpadButton_Y)) m |= PLAT_BTN_ZOOM_OUT;
  if (b & (HidNpadButton_R | HidNpadButton_X)) m |= PLAT_BTN_ZOOM_IN;
  if (b & HidNpadButton_ZL) m |= PLAT_BTN_FOLLOW;
  if (b & HidNpadButton_ZR) m |= PLAT_BTN_FONT;
  if (b & HidNpadButton_Up)    m |= PLAT_BTN_UP;
  if (b & HidNpadButton_Down)  m |= PLAT_BTN_DOWN;
  if (b & HidNpadButton_Left)  m |= PLAT_BTN_LEFT;
  if (b & HidNpadButton_Right) m |= PLAT_BTN_RIGHT;
  if (b & HidNpadButton_A) m |= PLAT_BTN_A | PLAT_BTN_SWKBD;
  if (b & HidNpadButton_B) m |= PLAT_BTN_B;
  if (b & HidNpadButton_X) m |= PLAT_BTN_X;
  if (b & HidNpadButton_Y) m |= PLAT_BTN_Y;
  return m;
}

void plat_poll_input(plat_input_t *out) {
  padUpdate(&pad);
  out->down = nx_map(padGetButtonsDown(&pad));
  out->held = nx_map(padGetButtons(&pad));

  HidAnalogStickState st = padGetStickPos(&pad, 0);
  out->pan_x = (st.x >  NX_STICK_DEADZONE) ?  1
             : (st.x < -NX_STICK_DEADZONE) ? -1 : 0;
  /* Screen y grows downward, the stick's does not. */
  out->pan_y = (st.y >  NX_STICK_DEADZONE) ? -1
             : (st.y < -NX_STICK_DEADZONE) ?  1 : 0;

  HidTouchScreenState ts;
  bool on_panel = false;
  int px = 0, py = 0;
  if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
    px = (int)ts.touches[0].x - PANEL_OFF_X;
    py = (int)ts.touches[0].y - PANEL_OFF_Y;
    on_panel = px >= 0 && px < PLAT_PANEL_W && py >= 0 && py < PLAT_PANEL_H;
  }

  static bool was_down = false;
  out->ptr_valid  = on_panel;
  out->ptr_down   = on_panel;
  out->ptr_tapped = on_panel && !was_down;
  out->ptr_x = (int16_t)px;
  out->ptr_y = (int16_t)py;
  was_down = on_panel;
}

/* ------------------------------------------------------------- keyboard -- */

/* hid reports the keyboard as a 256-bit bitmap of held usage ids rather than
   as events, so the edges have to be found here by diffing against the last
   frame. Auto-repeat is ours to do for the same reason - hid does not. */
#define NX_KBD_REPEAT_DELAY_US  400000
#define NX_KBD_REPEAT_RATE_US    35000

static u64 kbd_prev[4];
static u8  kbd_repeat_key;
static u64 kbd_repeat_at;

static void nx_kbd_emit(u8 usage, u64 mods) {
  char buf[8];
  const char *s = hid_term_bytes(usage,
                                 (mods & HidKeyboardModifier_Shift)    != 0,
                                 (mods & HidKeyboardModifier_Control)  != 0,
                                 (mods & HidKeyboardModifier_CapsLock) != 0,
                                 buf);
  if (s) rx_push_str(s);
}

void plat_poll_keyboard(void) {
  HidKeyboardState st;
  if (hidGetKeyboardStates(&st, 1) < 1) {
    caps.keyboard = false;
    return;
  }
  /* Nothing says "a keyboard is attached" directly, so the flag follows the
     first key ever seen: an empty report is indistinguishable from no
     keyboard at all until someone types. */
  u64 now = plat_us();

  for (int w = 0; w < 4; w++) {
    u64 fresh = st.keys[w] & ~kbd_prev[w];
    while (fresh) {
      int bit = __builtin_ctzll(fresh);
      fresh &= fresh - 1;
      u8 usage = (u8)(w * 64 + bit);
      /* The modifier keys themselves are in the bitmap too and produce
         nothing; st.modifiers is what carries their state. */
      if (usage >= 0xE0) continue;
      caps.keyboard = true;
      nx_kbd_emit(usage, st.modifiers);
      kbd_repeat_key = usage;
      kbd_repeat_at  = now + NX_KBD_REPEAT_DELAY_US;
    }
  }

  /* Repeat only the most recent key, and only while it is still down - the
     same thing every OS keyboard driver does. */
  if (kbd_repeat_key) {
    int w = kbd_repeat_key / 64, bit = kbd_repeat_key % 64;
    if (!(st.keys[w] & (1ull << bit))) {
      kbd_repeat_key = 0;
    } else if (now >= kbd_repeat_at) {
      nx_kbd_emit(kbd_repeat_key, st.modifiers);
      kbd_repeat_at = now + NX_KBD_REPEAT_RATE_US;
    }
  }

  memcpy(kbd_prev, st.keys, sizeof(kbd_prev));
}

/* -------------------------------------------------------------- threads -- */

static Thread emu_thread;
static bool   emu_started;

bool plat_thread_start(void (*entry)(void *), void *arg) {
  /* Core 2: cores 0 and 1 carry the main thread and the system's own work. */
  if (R_FAILED(threadCreate(&emu_thread, entry, arg, NULL, 128 * 1024, 0x2C, 2)))
    return false;
  if (R_FAILED(threadStart(&emu_thread))) { threadClose(&emu_thread); return false; }
  emu_started = true;
  return true;
}

void plat_thread_join(void) {
  if (!emu_started) return;
  threadWaitForExit(&emu_thread);
  threadClose(&emu_thread);
  emu_started = false;
}

const char *plat_thread_desc(void) { return "core 2"; }

void plat_mutex_init(plat_mutex_t *m)   { mutexInit((Mutex *)m); }
void plat_mutex_lock(plat_mutex_t *m)   { mutexLock((Mutex *)m); }
void plat_mutex_unlock(plat_mutex_t *m) { mutexUnlock((Mutex *)m); }

/* ------------------------------------------------------------------ net -- */

bool plat_net_init(void) { return R_SUCCEEDED(socketInitializeDefault()); }
void plat_net_exit(void) { socketExit(); }

/* -------------------------------------------------------------- entropy -- */

bool plat_random(void *buf, size_t len) { randomGet(buf, len); return true; }

/* -------------------------------------------------------------- sensors -- */

/* The six-axis sensors belong to a bound controller, which a handheld Switch
   has and a docked one may not, so the axes read 0 until that is wired up. */
void plat_sample_axes(int32_t *out) {
  memset(out, 0, sizeof(int32_t) * VI_NAXES);
}

/* ------------------------------------------------------------- 9P trees -- */

static const plat_tree_t trees[] = {
  { "sd", PLAT_SD, false },
};

int plat_v9p_trees(const plat_tree_t **out) {
  *out = trees;
  return (int)(sizeof(trees) / sizeof(trees[0]));
}

bool plat_v9p_mount(int idx) { return idx == 0; }   /* libnx mounts it for us */
void plat_v9p_unmount_all(void) {}
int64_t plat_v9p_free_bytes(void) { return -1; }

/* -------------------------------------------------------------- hw tree -- */

static int hw_rd_console_size(char *b, int n) {
  return snprintf(b, n, "%d %d\n", TERM_COLS, TERM_ROWS);
}
static int hw_rd_model(char *b, int n) { return snprintf(b, n, "%s\n", plat_model()); }

static int hw_rd_battery(char *b, int n) {
  u32 pct = 0;
  if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct)))
    return snprintf(b, n, "%lu\n", (unsigned long)pct);
  return snprintf(b, n, "unknown\n");
}

static int hw_rd_charging(char *b, int n) {
  PsmChargerType t = PsmChargerType_Unconnected;
  if (R_SUCCEEDED(psmGetChargerType(&t)))
    return snprintf(b, n, "%u\n", t != PsmChargerType_Unconnected ? 1u : 0u);
  return snprintf(b, n, "unknown\n");
}

static int hw_rd_firmware(char *b, int n) {
  SetSysFirmwareVersion v;
  if (R_SUCCEEDED(setsysGetFirmwareVersion(&v)))
    return snprintf(b, n, "%s\n", v.display_version);
  return snprintf(b, n, "unknown\n");
}

static int hw_rd_mode(char *b, int n) {
  return snprintf(b, n, "%s\n",
                  appletGetOperationMode() == AppletOperationMode_Console
                    ? "docked" : "handheld");
}

static int hw_rd_info(char *b, int n) {
  char batt[32], chg[32];
  hw_rd_battery(batt, sizeof(batt));
  hw_rd_charging(chg, sizeof(chg));
  for (char *p = batt; *p; p++) if (*p == '\n') *p = 0;
  for (char *p = chg;  *p; p++) if (*p == '\n') *p = 0;
  return snprintf(b, n,
      "model:    %s\nbattery:  %s%%\ncharging: %s\nconsole:  %dx%d\n",
      plat_model(), batt, chg, TERM_COLS, TERM_ROWS);
}

const plat_hw_ent plat_hw_files[] = {
  { "info",         0444, hw_rd_info,         NULL, HWS_NONE },
  { "model",        0444, hw_rd_model,        NULL, HWS_NONE },
  { "battery",      0444, hw_rd_battery,      NULL, HWS_NONE },
  { "charging",     0444, hw_rd_charging,     NULL, HWS_NONE },
  { "firmware",     0444, hw_rd_firmware,     NULL, HWS_NONE },
  { "mode",         0444, hw_rd_mode,         NULL, HWS_NONE },
  { "console_size", 0444, hw_rd_console_size, NULL, HWS_NONE },
};
const int plat_hw_count = (int)(sizeof(plat_hw_files) / sizeof(plat_hw_files[0]));

int plat_hw_camera(bool inner, uint8_t **frame) { (void)inner; (void)frame; return 0; }
int plat_hw_mic_read(uint8_t *out, int max)     { (void)out; (void)max; return 0; }
int plat_hw_audio_write(const uint8_t *d, int l) { (void)d; return l; }
