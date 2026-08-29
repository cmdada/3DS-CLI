/* Sony PlayStation Vita backend (VitaSDK). Implements source/core/plat.h.
 *
 * One 960x544 screen, split: guest console on the top 352 rows, panel on the
 * bottom 192, so the keyboard and the settings page are always up the same as
 * they are on a console that actually has two screens.
 *
 * Four Cortex-A9 cores and three of them are ours, so the emulator gets a
 * whole core to itself instead of elbowing the redraw out of the way like it
 * has to on the psp. nicest console in here to write a backend for tbh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/motion.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#include "plat.h"

/* newlib takes the whole malloc heap as one memblock sized by this weak
   symbol, so the guest's RAM has to be asked for way up here instead of found
   later
   */
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;

_Static_assert(sizeof(SceUID) <= PLAT_MUTEX_SIZE, "PLAT_MUTEX_SIZE too small for SceUID");

static plat_caps_t caps;
static char        g_model[64];

/* ---------------------------------------------------------------- video -- */

/* sceDisplaySetFrameBuf wants a pitch that's a multiple of 64 pixels, which
   960 already is, and a CDRAM block aligned to 256KB.

   CDRAM and not plain USER_RW because THERE IS NO USERLAND CACHE MAINTENANCE
   CALL ON THIS CONSOLE, psp2/kernel/cpu.h is atomics and nothing else. so a
   cached framebuffer could never be made coherent with the display the way
   sceKernelDcacheWritebackAll does it on psp. CDRAM comes back uncached which
   costs the blit some write bandwidth, i'll take slow over garbage on screen */
#define VITA_FB_PITCH_PX VITA_SCREEN_W
#define VITA_FB_PITCH    (VITA_FB_PITCH_PX * 4)
#define VITA_FB_BYTES    (((size_t)VITA_FB_PITCH * VITA_SCREEN_H + 0x3FFFF) & ~(size_t)0x3FFFF)

static SceUID   vita_fb_uid = -1;
static uint8_t *vita_fb;

/* Single-buffered like the psp, not double like the switch. */
bool plat_surface(plat_surf s, plat_fb_t *out) {
  if (!vita_fb) return false;
  out->x_stride = 4;
  out->y_stride = VITA_FB_PITCH;
  out->bpp      = 4;
  if (s == PLAT_SURF_TERM) {
    out->base = vita_fb;
    out->w    = PLAT_TERM_W;
    out->h    = PLAT_TERM_H;
    return true;
  }
  if (s == PLAT_SURF_PANEL) {
    out->base = vita_fb + (size_t)PLAT_TERM_H * VITA_FB_PITCH;
    out->w    = PLAT_PANEL_W;
    out->h    = PLAT_PANEL_H;
    return true;
  }
  return false;
}

/* Scanned out in place so there's nothing to actually push, the wait is only
   here so a tight redraw loop doesn't spin */
void plat_present(unsigned mask) {
  if (!mask || !vita_fb) return;
  sceDisplayWaitVblankStart();
}

/* ---------------------------------------------------------------- touch -- */

/* The front panel reports in its own space, 1920x1088 on every model i know
   of, but ask the panel for the scale instead of hardcoding it  */
static int touch_min_x, touch_min_y;
static int touch_den_x = 1, touch_den_y = 1;
static bool touch_ok;

static void vita_touch_init(void) {
  SceTouchPanelInfo info;
  if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &info) < 0) return;

  int span_x = (int)info.maxAaX - (int)info.minAaX;
  int span_y = (int)info.maxAaY - (int)info.minAaY;
  if (span_x <= 0 || span_y <= 0) return;

  touch_min_x = info.minAaX;
  touch_min_y = info.minAaY;
  touch_den_x = span_x;
  touch_den_y = span_y;

  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  touch_ok = true;
}

/* ----------------------------------------------------------------- init -- */

bool plat_init(void) {
  vita_fb_uid = sceKernelAllocMemBlock("3dscli_fb",
                                       SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                       VITA_FB_BYTES, NULL);
  if (vita_fb_uid < 0) return false;
  if (sceKernelGetMemBlockBase(vita_fb_uid, (void **)&vita_fb) < 0) return false;
  memset(vita_fb, 0, VITA_FB_BYTES);

  SceDisplayFrameBuf fb = {
    .size        = sizeof(fb),
    .base        = vita_fb,
    .pitch       = VITA_FB_PITCH_PX,
    .pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
    .width       = VITA_SCREEN_W,
    .height      = VITA_SCREEN_H,
  };
  if (sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE) < 0) return false;

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  vita_touch_init();

  /* Both sensors come up together or not at all. */
  caps.sensors = sceMotionStartSampling() >= 0;

  /* 444MHz */
  caps.speedup = scePowerSetArmClockFrequency(444) >= 0;

  SceKernelSystemSwVersion sw;
  memset(&sw, 0, sizeof(sw));
  sw.size = sizeof(sw);
  const char *ver = (sceKernelGetSystemSwVersion(&sw) >= 0 && sw.versionString[0])
                      ? sw.versionString : "unknown";
  snprintf(g_model, sizeof(g_model), "%s (fw %s, %dMHz)",
           sceKernelIsPSVitaTV() ? "PS TV" : "Vita",
           ver, scePowerGetArmClockFrequency());

  caps.emu_thread = true;
  caps.pointer    = touch_ok;
  caps.net        = true;
  caps.rng        = true;    /* sceKernelGetRandomNumber is always available */
  caps.audio      = false;
  caps.camera     = false;   /* SceCamera needs a privileged module          */
  caps.mic        = false;
  /* SceIme exists but it's a modal that draws itself over the whole screen,
     same reason i didn't use the psp osk */
  caps.swkbd      = false;
  return true;
}

void plat_exit(void) {
  if (caps.sensors) sceMotionStopSampling();
  if (vita_fb_uid >= 0) sceKernelFreeMemBlock(vita_fb_uid);
  sceKernelExitProcess(0);
}

/* Nothing suspends vita homebrew the way an applet transition does on the
   switch, so this only ends when the user says so */
bool plat_running(void) { return true; }

const plat_caps_t *plat_caps(void) { return &caps; }
const char *plat_model(void) { return g_model; }

/* Emulator has a core all to itself so the redraw and the input poll cost the
   guest nothing, crank it */
void plat_ui_cadence(uint32_t *redraw_us, uint32_t *poll_us) {
  *redraw_us = 16666;          /* 60fps */
  *poll_us   = 1000000u / 60;
}

/* ----------------------------------------------------------------- time -- */

uint64_t plat_us(void) { return (uint64_t)sceKernelGetProcessTimeWide(); }
uint64_t plat_wallclock_ms(void) { return (uint64_t)time(NULL) * 1000ull; }

void plat_sleep_us(uint64_t us) {
  while (us > 0xF0000000ull) { sceKernelDelayThread(0xF0000000u); us -= 0xF0000000ull; }
  sceKernelDelayThread((SceUInt)us);
}

/* ---------------------------------------------------------------- input -- */

/* Sticks read 0..255 centered near 128 with a fair bit of slop */
#define VITA_STICK_DEADZONE 32

static uint32_t vita_map(unsigned int b) {
  uint32_t m = 0;
  if (b & SCE_CTRL_START)    m |= PLAT_BTN_QUIT;
  if (b & SCE_CTRL_SELECT)   m |= PLAT_BTN_SETTINGS;
  if (b & SCE_CTRL_LTRIGGER) m |= PLAT_BTN_ZOOM_OUT;
  if (b & SCE_CTRL_RTRIGGER) m |= PLAT_BTN_ZOOM_IN;
  if (b & SCE_CTRL_UP)    m |= PLAT_BTN_UP;
  if (b & SCE_CTRL_DOWN)  m |= PLAT_BTN_DOWN;
  if (b & SCE_CTRL_LEFT)  m |= PLAT_BTN_LEFT;
  if (b & SCE_CTRL_RIGHT) m |= PLAT_BTN_RIGHT;
  /* Cross confirms and circle goes back. no ZL/ZR on this thing so follow and
     font ride on the face buttons like they do on psp */
  if (b & SCE_CTRL_CROSS)    m |= PLAT_BTN_A;
  if (b & SCE_CTRL_CIRCLE)   m |= PLAT_BTN_B;
  if (b & SCE_CTRL_SQUARE)   m |= PLAT_BTN_X | PLAT_BTN_FONT;
  if (b & SCE_CTRL_TRIANGLE) m |= PLAT_BTN_Y | PLAT_BTN_FOLLOW;
  return m;
}

void plat_poll_input(plat_input_t *out) {
  memset(out, 0, sizeof(*out));

  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  /* Peek and not Read, Read blocks until the next sample */
  sceCtrlPeekBufferPositive(0, &pad, 1);

  static unsigned int prev = 0;
  out->held = vita_map(pad.buttons);
  out->down = vita_map(pad.buttons & ~prev);
  prev = pad.buttons;

  int dx = (int)pad.lx - 128, dy = (int)pad.ly - 128;
  out->pan_x = (dx > VITA_STICK_DEADZONE || dx < -VITA_STICK_DEADZONE) ? dx : 0;
  out->pan_y = (dy > VITA_STICK_DEADZONE || dy < -VITA_STICK_DEADZONE) ? dy : 0;

  bool on_panel = false;
  int px = 0, py = 0;
  if (touch_ok) {
    SceTouchData td;
    memset(&td, 0, sizeof(td));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) >= 0 && td.reportNum > 0) {
      /* Panel space to screen space, then screen space to panel-local. */
      int sx = ((int)td.report[0].x - touch_min_x) * VITA_SCREEN_W / touch_den_x;
      int sy = ((int)td.report[0].y - touch_min_y) * VITA_SCREEN_H / touch_den_y;
      px = sx;
      py = sy - PLAT_TERM_H;
      on_panel = px >= 0 && px < PLAT_PANEL_W && py >= 0 && py < PLAT_PANEL_H;
    }
  }

  static bool was_down = false;
  out->ptr_valid  = on_panel;
  out->ptr_down   = on_panel;
  out->ptr_tapped = on_panel && !was_down;
  out->ptr_x = (int16_t)px;
  out->ptr_y = (int16_t)py;
  was_down = on_panel;
}

/* Same as the PSP: the port is device-only, so there is no keyboard to
   host. See plat_poll_keyboard in source/core/plat.h. */
void plat_poll_keyboard(void) {}

/* -------------------------------------------------------------- threads -- */

static SceUID emu_tid = -1;
static void (*emu_entry)(void *);
static void  *emu_arg;

static int vita_emu_trampoline(SceSize args, void *argp) {
  (void)args; (void)argp;
  emu_entry(emu_arg);
  return 0;
}

/* Pinned to user core 2 so the main thread and the system's own work keep 0
   and 1, same split as the switch backend and for the same reason */
bool plat_thread_start(void (*entry)(void *), void *arg) {
  emu_entry = entry;
  emu_arg   = arg;

  emu_tid = sceKernelCreateThread("3dscli_emu", vita_emu_trampoline, 0x40,
                                  128 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
  if (emu_tid < 0) { emu_tid = -1; return false; }
  if (sceKernelStartThread(emu_tid, 0, NULL) < 0) {
    sceKernelDeleteThread(emu_tid);
    emu_tid = -1;
    return false;
  }
  return true;
}

void plat_thread_join(void) {
  if (emu_tid < 0) return;
  sceKernelWaitThreadEnd(emu_tid, NULL, NULL);
  sceKernelDeleteThread(emu_tid);
  emu_tid = -1;
}

const char *plat_thread_desc(void) { return "core 2"; }

void plat_mutex_init(plat_mutex_t *m) {
  SceUID s = sceKernelCreateMutex("3dscli_lock", 0, 0, NULL);
  memcpy(m->opaque, &s, sizeof(s));
}

void plat_mutex_lock(plat_mutex_t *m) {
  SceUID s; memcpy(&s, m->opaque, sizeof(s));
  sceKernelLockMutex(s, 1, NULL);
}

void plat_mutex_unlock(plat_mutex_t *m) {
  SceUID s; memcpy(&s, m->opaque, sizeof(s));
  sceKernelUnlockMutex(s, 1);
}

/* ------------------------------------------------------------------ net -- */

static bool  net_up;
static bool  net_ours;   /* we called sceNetInit, so we owe it a sceNetTerm */
static void *net_pool;

#define VITA_NET_POOL (1024 * 1024)

static void vita_net_teardown(void) {
  if (net_ours) { sceNetTerm(); net_ours = false; }
  free(net_pool);
  net_pool = NULL;
}

/* Joins whatever AP the user already set up in the vita's own settings.

   Blocking: association plus DHCP takes seconds and this runs on whichever
   thread asked for the device. */
bool plat_net_init(void) {
  if (net_up) return true;

  if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return false;

  net_pool = malloc(VITA_NET_POOL);
  if (!net_pool) return false;

  /* If the shell already brought the stack up this fails exactly like a real
     error would
      */
  SceNetInitParam p = { net_pool, VITA_NET_POOL, 0 };
  net_ours = sceNetInit(&p) >= 0;

  if (sceNetCtlInit() < 0) { vita_net_teardown(); return false; }

  /* The console associates on its own, this just waits around for it */
  for (int waited = 0; waited < 15000; waited += 50) {
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0) break;
    if (state == SCE_NETCTL_STATE_CONNECTED) { net_up = true; return true; }
    sceKernelDelayThread(50 * 1000);
  }

  sceNetCtlTerm();
  vita_net_teardown();
  return false;
}

void plat_net_exit(void) {
  if (!net_up) return;
  sceNetCtlTerm();
  vita_net_teardown();
  net_up = false;
}

/* -------------------------------------------------------------- entropy -- */

bool plat_random(void *buf, size_t len) {
  return sceKernelGetRandomNumber(buf, (SceSize)len) >= 0;
}

/* -------------------------------------------------------------- sensors -- */

/* SceMotion hands back floats, accelerometer in G and gyro in radians/sec,
  */
#define VITA_ACCEL_SCALE 8192.0f
#define VITA_GYRO_SCALE  1000.0f

static int32_t clamp16(float v) {
  if (v >  32767.0f) return  32767;
  if (v < -32768.0f) return -32768;
  return (int32_t)v;
}

void plat_sample_axes(int32_t *out) {
  memset(out, 0, sizeof(int32_t) * VI_NAXES);
  if (!caps.sensors) return;

  SceMotionSensorState st;
  memset(&st, 0, sizeof(st));
  if (sceMotionGetSensorState(&st, 1) < 0) return;

  out[0] = clamp16(st.accelerometer.x * VITA_ACCEL_SCALE);
  out[1] = clamp16(st.accelerometer.y * VITA_ACCEL_SCALE);
  out[2] = clamp16(st.accelerometer.z * VITA_ACCEL_SCALE);
  out[3] = clamp16(st.gyro.x * VITA_GYRO_SCALE);
  out[4] = clamp16(st.gyro.y * VITA_GYRO_SCALE);
  out[5] = clamp16(st.gyro.z * VITA_GYRO_SCALE);
  /* No 3D slider and no volume readback, so both console-specific axes stay
     at 0 rather than reporting something invented. */
}

/* ------------------------------------------------------------- 9P trees -- */

/* Named "sd" rather than "ux0" like every other console's, because the guest's
   own mount script names it and one rootfs boots on all of them. */
static const plat_tree_t trees[] = {
  { "sd", PLAT_SD, false },
};

int plat_v9p_trees(const plat_tree_t **out) {
  *out = trees;
  return (int)(sizeof(trees) / sizeof(trees[0]));
}

/* system mounts ux0: for us, this is only checking it's really there :p
   a vita tv with nothing in it still reports the path but won't open it */
bool plat_v9p_mount(int idx) {
  if (idx != 0) return false;
  DIR *d = opendir(PLAT_SD);
  if (!d) return false;
  closedir(d);
  return true;
}

void plat_v9p_unmount_all(void) {}

/* the vita just tells us, no devctl incantation like the psp needed */
int64_t plat_v9p_free_bytes(void) {
  uint64_t max_size = 0, free_size = 0;
  if (sceAppMgrGetDevInfo("ux0:", &max_size, &free_size) < 0) return -1;
  return (int64_t)free_size;
}

/* -------------------------------------------------------------- hw tree -- */

static int hw_rd_console_size(char *b, int n) {
  return snprintf(b, n, "%d %d\n", TERM_COLS, TERM_ROWS);
}
static int hw_rd_model(char *b, int n) { return snprintf(b, n, "%s\n", g_model); }

static int hw_rd_battery(char *b, int n) {
  int pct = scePowerGetBatteryLifePercent();
  if (pct < 0) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%d\n", pct);
}

static int hw_rd_charging(char *b, int n) {
  return snprintf(b, n, "%d\n", scePowerIsBatteryCharging() ? 1 : 0);
}

static int hw_rd_ac(char *b, int n) {
  return snprintf(b, n, "%d\n", scePowerIsPowerOnline() ? 1 : 0);
}

static int hw_rd_battery_time(char *b, int n) {
  int mins = scePowerGetBatteryLifeTime();
  if (mins < 0) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%d\n", mins);
}

static int hw_rd_battery_temp(char *b, int n) {
  return snprintf(b, n, "%d\n", scePowerGetBatteryTemp());
}

static int hw_rd_cpu_mhz(char *b, int n) {
  return snprintf(b, n, "%d\n", scePowerGetArmClockFrequency());
}

static int hw_rd_firmware(char *b, int n) {
  SceKernelSystemSwVersion sw;
  memset(&sw, 0, sizeof(sw));
  sw.size = sizeof(sw);
  if (sceKernelGetSystemSwVersion(&sw) < 0 || !sw.versionString[0])
    return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%s\n", sw.versionString);
}

static int hw_rd_net(char *b, int n) {
  int state = 0;
  if (sceNetCtlInetGetState(&state) < 0) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%d\n", state == SCE_NETCTL_STATE_CONNECTED ? 1 : 0);
}

static int hw_rd_info(char *b, int n) {
  return snprintf(b, n,
      "model:    %s\nbattery:  %d%%\ncharging: %d\nscreen:   %dx%d\nconsole:  %dx%d\n",
      g_model, scePowerGetBatteryLifePercent(),
      scePowerIsBatteryCharging() ? 1 : 0,
      VITA_SCREEN_W, VITA_SCREEN_H, TERM_COLS, TERM_ROWS);
}

const plat_hw_ent plat_hw_files[] = {
  { "info",         0444, hw_rd_info,         NULL, HWS_NONE },
  { "model",        0444, hw_rd_model,        NULL, HWS_NONE },
  { "battery",      0444, hw_rd_battery,      NULL, HWS_NONE },
  { "battery_time", 0444, hw_rd_battery_time, NULL, HWS_NONE },
  { "battery_temp", 0444, hw_rd_battery_temp, NULL, HWS_NONE },
  { "charging",     0444, hw_rd_charging,     NULL, HWS_NONE },
  { "ac",           0444, hw_rd_ac,           NULL, HWS_NONE },
  { "cpu_mhz",      0444, hw_rd_cpu_mhz,      NULL, HWS_NONE },
  { "network",      0444, hw_rd_net,          NULL, HWS_NONE },
  { "firmware",     0444, hw_rd_firmware,     NULL, HWS_NONE },
  { "console_size", 0444, hw_rd_console_size, NULL, HWS_NONE },
};
const int plat_hw_count = (int)(sizeof(plat_hw_files) / sizeof(plat_hw_files[0]));

int plat_hw_camera(bool inner, uint8_t **frame) { (void)inner; (void)frame; return 0; }
int plat_hw_mic_read(uint8_t *out, int max)     { (void)out; (void)max; return 0; }
int plat_hw_audio_write(const uint8_t *d, int l) { (void)d; return l; }
