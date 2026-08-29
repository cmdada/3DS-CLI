/* Sony PlayStation Portable backend (pspdev + pspsdk). Implements
 * source/core/plat.h.
 * The one core is shared: unlike the Wii and GameCube this console has a
 * preemptive scheduler worth using, so the emulator does get a thread, just a
 * lower-priority one. See plat_thread_start for why that is the whole trick.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <psputility.h>
#include <pspwlan.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>

#include "plat.h"

PSP_MODULE_INFO("3DS-CLI", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

PSP_HEAP_SIZE_KB(-2048);

_Static_assert(sizeof(SceUID) <= PLAT_MUTEX_SIZE, "PLAT_MUTEX_SIZE too small for SceUID");

static plat_caps_t caps;
static char        g_model[64];

/* ------------------------------------------------------------ libc gaps -- */

/* This console's newlib has no ftruncate, and pspsdk's own truncate() is not
   a substitute: it reads the whole file into a VLA on the stack, which for
   the ~200MB rootfs is a stack overflow rather than a slow path.
 */
int ftruncate(int fd, off_t length) {
  if (length <= 0) return -1;
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, length - 1, SEEK_SET) < 0) return -1;
  const char zero = 0;
  bool ok = write(fd, &zero, 1) == 1;
  if (lseek(fd, cur, SEEK_SET) < 0) return -1;
  return ok ? 0 : -1;
}

/* ----------------------------------------------------------------- life -- */

/* Cleared by the exit callback */
static volatile bool psp_running = true;

static int psp_exit_cb(int arg1, int arg2, void *common) {
  (void)arg1; (void)arg2; (void)common;
  psp_running = false;
  return 0;
}

/* Callbacks are only delivered to a thread sleeping in a *_CB call :( */
static int psp_cb_thread(SceSize args, void *argp) {
  (void)args; (void)argp;
  int cbid = sceKernelCreateCallback("3dscli_exit", psp_exit_cb, NULL);
  if (cbid >= 0) sceKernelRegisterExitCallback(cbid);
  sceKernelSleepThreadCB();
  return 0;
}

/* ---------------------------------------------------------------- video -- */

/* sceDisplaySetFrameBuf wants a line stride that is a multiple of 64 pixels */
#define PSP_FB_STRIDE_PX 512
#define PSP_FB_PITCH     (PSP_FB_STRIDE_PX * 4)
#define PSP_FB_BYTES     ((size_t)PSP_FB_PITCH * PSP_SCREEN_H)

/* Two mappings of the same VRAM, and which surface gets which is the point. */
static uint8_t *psp_vram;      /* cached   - the terminal */
static uint8_t *psp_vram_uc;   /* uncached - the panel    */

/* The uncached mirror of a VRAM address. */
#define PSP_VRAM_UNCACHED(p) ((uint8_t *)(0x40000000u | (uintptr_t)(p)))

_Static_assert((((size_t)PLAT_TERM_H * PSP_FB_PITCH) % 64) == 0,
               "terminal/panel split must fall on a cache line boundary");

/* Single-buffered */
bool plat_surface(plat_surf s, plat_fb_t *out) {
  if (!psp_vram) return false;
  out->x_stride = 4;
  out->y_stride = PSP_FB_PITCH;
  out->bpp      = 4;
  if (s == PLAT_SURF_TERM) {
    out->base = psp_vram;
    out->w    = PLAT_TERM_W;
    out->h    = PLAT_TERM_H;
    return true;
  }
  if (s == PLAT_SURF_PANEL) {
    out->base = psp_vram_uc + (size_t)PLAT_TERM_H * PSP_FB_PITCH;
    out->w    = PLAT_PANEL_W;
    out->h    = PLAT_PANEL_H;
    return true;
  }
  return false;
}

void plat_present(unsigned mask) {
  if (!mask || !psp_vram) return;
  /* Flushing the whole 16KB data cache */
  sceKernelDcacheWritebackAll();
  sceDisplayWaitVblankStart();
}

/* ----------------------------------------------------------------- init -- */

bool plat_init(void) {
  int cb = sceKernelCreateThread("3dscli_cb", psp_cb_thread, 0x11, 0xFA0, 0, NULL);
  if (cb >= 0) sceKernelStartThread(cb, 0, NULL);

  psp_vram = (uint8_t *)sceGeEdramGetAddr();
  if (!psp_vram) return false;
  psp_vram_uc = PSP_VRAM_UNCACHED(psp_vram);
  memset(psp_vram, 0, PSP_FB_BYTES);
  sceKernelDcacheWritebackAll();

  sceDisplaySetMode(0, PSP_SCREEN_W, PSP_SCREEN_H);
  sceDisplaySetFrameBuf(psp_vram, PSP_FB_STRIDE_PX,
                        PSP_DISPLAY_PIXEL_FORMAT_8888,
                        PSP_DISPLAY_SETBUF_IMMEDIATE);

  sceCtrlSetSamplingCycle(0);            /* sample on the display's vblank */
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

  /* i dont care about battery life lets gooooo overclocked psp*/
  caps.speedup = scePowerSetClockFrequency(333, 333, 166) == 0;

  unsigned int fw = sceKernelDevkitVersion();
  snprintf(g_model, sizeof(g_model), "PSP (fw %u.%u%u, %dMHz)",
           (fw >> 24) & 0xff, (fw >> 16) & 0xff, (fw >> 8) & 0xff,
           (int)scePowerGetCpuClockFrequency());

  caps.emu_thread = true;
  caps.pointer    = false;   /* no touchscreen: the keyboard uses D-pad focus */
  caps.net        = true;
  caps.rng        = false;   /* no user-mode CSPRNG; virtio_rng.h falls back  */
  caps.sensors    = false;   /* no accelerometer, no gyro, no sliders         */
  caps.audio      = false;
  caps.camera     = false;   /* the Go!Cam is an accessory almost nobody has  */
  caps.mic        = false;
  /* sceUtilityOsk exists, but it is a modal dialog that renders itself */
  caps.swkbd      = false;
  return true;
}

void plat_exit(void) {
  sceKernelExitGame();
}

bool plat_running(void) { return psp_running; }

const plat_caps_t *plat_caps(void) { return &caps; }
const char *plat_model(void) { return g_model; }

/* The emulator shares the only core with the redraw and the input poll, so
   both run like my 3ds conf*/
void plat_ui_cadence(uint32_t *redraw_us, uint32_t *poll_us) {
  *redraw_us = 120000;          /* ~8fps */
  *poll_us   = 1000000u / 30;
}

/* ----------------------------------------------------------------- time -- */

uint64_t plat_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }
uint64_t plat_wallclock_ms(void) { return (uint64_t)time(NULL) * 1000ull; }

void plat_sleep_us(uint64_t us) {
  while (us > 0xF0000000ull) { sceKernelDelayThread(0xF0000000u); us -= 0xF0000000ull; }
  sceKernelDelayThread((SceUInt)us);
}

/* ---------------------------------------------------------------- input -- */

/* The nub reads 0..255 with its centre near 128 and a good deal of slop
   around it; this is the hardware's, not a UI preference. 
   
   also lowkey this is the first time I've typed slop in a while in a 
   non-ai matter, yay! the world is doomed */
#define PSP_NUB_DEADZONE 32

static uint32_t psp_map(unsigned int b) {
  uint32_t m = 0;
  if (b & PSP_CTRL_START)    m |= PLAT_BTN_QUIT;
  if (b & PSP_CTRL_SELECT)   m |= PLAT_BTN_SETTINGS;
  if (b & PSP_CTRL_LTRIGGER) m |= PLAT_BTN_ZOOM_OUT;
  if (b & PSP_CTRL_RTRIGGER) m |= PLAT_BTN_ZOOM_IN;
  if (b & PSP_CTRL_UP)    m |= PLAT_BTN_UP;
  if (b & PSP_CTRL_DOWN)  m |= PLAT_BTN_DOWN;
  if (b & PSP_CTRL_LEFT)  m |= PLAT_BTN_LEFT;
  if (b & PSP_CTRL_RIGHT) m |= PLAT_BTN_RIGHT;
  /* Cross confirms and circle goes back */
  if (b & PSP_CTRL_CROSS)    m |= PLAT_BTN_A;
  if (b & PSP_CTRL_CIRCLE)   m |= PLAT_BTN_B;
  if (b & PSP_CTRL_SQUARE)   m |= PLAT_BTN_X | PLAT_BTN_FONT;
  if (b & PSP_CTRL_TRIANGLE) m |= PLAT_BTN_Y | PLAT_BTN_FOLLOW;
  return m;
}

void plat_poll_input(plat_input_t *out) {
  memset(out, 0, sizeof(*out));

  SceCtrlData pad;
  /* Peek, not Read: Read blocks until the next sample, and this runs on the
     thread the emulator is waiting behind. */
  sceCtrlPeekBufferPositive(&pad, 1);

  static unsigned int prev = 0;
  out->held = psp_map(pad.Buttons);
  out->down = psp_map(pad.Buttons & ~prev);
  prev = pad.Buttons;

  int dx = (int)pad.Lx - 128, dy = (int)pad.Ly - 128;
  out->pan_x = (dx > PSP_NUB_DEADZONE || dx < -PSP_NUB_DEADZONE) ? dx : 0;
  out->pan_y = (dy > PSP_NUB_DEADZONE || dy < -PSP_NUB_DEADZONE) ? dy : 0;

  /* No pointer of any kind on the psp, i wish there was one tho */
  out->ptr_valid = false;
}

/* The PSP's USB port is device-only: it can be a drive on a PC, but it
   cannot host a keyboard. See plat_poll_keyboard in source/core/plat.h. */
void plat_poll_keyboard(void) {}

/* -------------------------------------------------------------- threads -- */

static SceUID emu_tid = -1;
static void (*emu_entry)(void *);
static void  *emu_arg;
static char   emu_desc[32] = "none";

static int psp_emu_trampoline(SceSize args, void *argp) {
  (void)args; (void)argp;
  emu_entry(emu_arg);
  return 0;
}

/* Deliberately *below* the main thread's priority, which is the whole reason
   this console gets a thread where the Wii and GameCube do not.
 */
bool plat_thread_start(void (*entry)(void *), void *arg) {
  emu_entry = entry;
  emu_arg   = arg;

  int prio = sceKernelGetThreadCurrentPriority();
  emu_tid = sceKernelCreateThread("3dscli_emu", psp_emu_trampoline, prio + 8,
                                  64 * 1024, PSP_THREAD_ATTR_USER, NULL);
  if (emu_tid < 0) { emu_tid = -1; return false; }
  if (sceKernelStartThread(emu_tid, 0, NULL) < 0) {
    sceKernelDeleteThread(emu_tid);
    emu_tid = -1;
    return false;
  }
  snprintf(emu_desc, sizeof(emu_desc), "priority %d", prio + 8);
  return true;
}

void plat_thread_join(void) {
  if (emu_tid < 0) return;
  sceKernelWaitThreadEnd(emu_tid, NULL);
  sceKernelDeleteThread(emu_tid);
  emu_tid = -1;
}

const char *plat_thread_desc(void) { return emu_desc; }

void plat_mutex_init(plat_mutex_t *m) {
  SceUID s = sceKernelCreateSema("3dscli_lock", 0, 1, 1, NULL);
  memcpy(m->opaque, &s, sizeof(s));
}

void plat_mutex_lock(plat_mutex_t *m) {
  SceUID s; memcpy(&s, m->opaque, sizeof(s));
  sceKernelWaitSema(s, 1, NULL);
}

void plat_mutex_unlock(plat_mutex_t *m) {
  SceUID s; memcpy(&s, m->opaque, sizeof(s));
  sceKernelSignalSema(s, 1);
}

/* ------------------------------------------------------------------ net -- */

static bool net_up;

/* Joins whichever access point the user configured in the PSP's own settings

   Blocking: connecting takes seconds and this runs on whichever thread asked
   for the device. */
bool plat_net_init(void) {
  if (net_up) return true;

  /* The physical WLAN switch. */
  if (!sceWlanGetSwitchState()) return false;

  if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0) return false;
  if (sceUtilityLoadNetModule(PSP_NET_MODULE_INET) < 0)   return false;

  if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) < 0) return false;
  if (sceNetInetInit() < 0)    return false;
  if (sceNetResolverInit() < 0) return false;
  if (sceNetApctlInit(0x8000, 48) < 0) return false;
  if (sceNetApctlConnect(1) < 0) return false;

  /* Association plus DHCP. Fifteen seconds is a while */
  for (int waited = 0; waited < 15000; waited += 50) {
    int state = 0;
    if (sceNetApctlGetState(&state) < 0) break;
    if (state == PSP_NET_APCTL_STATE_GOT_IP) { net_up = true; return true; }
    if (state == PSP_NET_APCTL_STATE_DISCONNECTED && waited > 1000) break;
    sceKernelDelayThread(50 * 1000);
  }

  sceNetApctlDisconnect();
  return false;
}

void plat_net_exit(void) {
  if (!net_up) return;
  sceNetApctlDisconnect();
  sceNetApctlTerm();
  sceNetResolverTerm();
  sceNetInetTerm();
  sceNetTerm();
  net_up = false;
}

/* -------------------------------------------------------------- entropy -- */

/* Nothing in user mode gets at the KIRK engine, and sceKernelUtilsMt19937 is
   a Mersenne Twister seeded by the caller, not a CSPRNG and no better than
   what virtio_rng.h falls back to on its own. */
bool plat_random(void *buf, size_t len) { (void)buf; (void)len; return false; }

/* -------------------------------------------------------------- sensors -- */

void plat_sample_axes(int32_t *out) {
  memset(out, 0, sizeof(int32_t) * VI_NAXES);
}

/* ------------------------------------------------------------- 9P trees -- */

/* Named "sd" rather than "ms" like every other console's, because the guest's
   own mount script names it and one rootfs boots on all of them. */
static const plat_tree_t trees[] = {
  { "sd", PLAT_SD, false },
};

int plat_v9p_trees(const plat_tree_t **out) {
  *out = trees;
  return (int)(sizeof(trees) / sizeof(trees[0]));
}

/*
just to check if there is a card :p
*/
bool plat_v9p_mount(int idx) {
  if (idx != 0) return false;
  DIR *d = opendir(PLAT_SD);
  if (!d) return false;
  closedir(d);
  return true;
}

void plat_v9p_unmount_all(void) {}

/* The Memory Stick's free space, the psp actually tells us! */
int64_t plat_v9p_free_bytes(void) {
  struct {
    unsigned int max_clusters, free_clusters, max_sectors, sector_size, sector_count;
  } info;
  void *pinfo = &info;
  memset(&info, 0, sizeof(info));
  if (sceIoDevctl("ms0:", 0x02425818, &pinfo, sizeof(pinfo), NULL, 0) < 0) return -1;
  return (int64_t)info.free_clusters * info.sector_count * info.sector_size;
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

static int hw_rd_battery_volt(char *b, int n) {
  return snprintf(b, n, "%d\n", scePowerGetBatteryVolt());
}

static int hw_rd_cpu_mhz(char *b, int n) {
  return snprintf(b, n, "%d %d\n", (int)scePowerGetCpuClockFrequency(),
                  (int)scePowerGetBusClockFrequency());
}

static int hw_rd_wlan(char *b, int n) {
  return snprintf(b, n, "%d\n", sceWlanGetSwitchState() ? 1 : 0);
}

static int hw_rd_firmware(char *b, int n) {
  unsigned int v = sceKernelDevkitVersion();
  return snprintf(b, n, "%u.%u%u\n",
                  (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff);
}

static int hw_rd_info(char *b, int n) {
  return snprintf(b, n,
      "model:    %s\nbattery:  %d%%\ncharging: %d\nscreen:   %dx%d\nconsole:  %dx%d\n",
      g_model, scePowerGetBatteryLifePercent(),
      scePowerIsBatteryCharging() ? 1 : 0,
      PSP_SCREEN_W, PSP_SCREEN_H, TERM_COLS, TERM_ROWS);
}

const plat_hw_ent plat_hw_files[] = {
  { "info",         0444, hw_rd_info,         NULL, HWS_NONE },
  { "model",        0444, hw_rd_model,        NULL, HWS_NONE },
  { "battery",      0444, hw_rd_battery,      NULL, HWS_NONE },
  { "battery_time", 0444, hw_rd_battery_time, NULL, HWS_NONE },
  { "battery_temp", 0444, hw_rd_battery_temp, NULL, HWS_NONE },
  { "battery_volt", 0444, hw_rd_battery_volt, NULL, HWS_NONE },
  { "charging",     0444, hw_rd_charging,     NULL, HWS_NONE },
  { "ac",           0444, hw_rd_ac,           NULL, HWS_NONE },
  { "cpu_mhz",      0444, hw_rd_cpu_mhz,      NULL, HWS_NONE },
  { "wlan_switch",  0444, hw_rd_wlan,         NULL, HWS_NONE },
  { "firmware",     0444, hw_rd_firmware,     NULL, HWS_NONE },
  { "console_size", 0444, hw_rd_console_size, NULL, HWS_NONE },
};
const int plat_hw_count = (int)(sizeof(plat_hw_files) / sizeof(plat_hw_files[0]));

int plat_hw_camera(bool inner, uint8_t **frame) { (void)inner; (void)frame; return 0; }
int plat_hw_mic_read(uint8_t *out, int max)     { (void)out; (void)max; return 0; }
int plat_hw_audio_write(const uint8_t *d, int l) { (void)d; return l; }
