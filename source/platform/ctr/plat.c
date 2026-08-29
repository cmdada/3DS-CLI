/* Nintendo 3DS backend (devkitARM + libctru). Implements source/core/plat.h. */

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "plat.h"

/* Serializes HIDUSER_GetSoundVolume, the one HID IPC call issued from both
   threads: plat_sample_axes on the main thread and the hw/slider_volume
   handler, which the guest reaches on the emulation thread through 9P.
   libctru's HID session handle is a single global, and while Horizon IPC
   sessions are generally fine with concurrent callers, serializing this rare,
   cheap call costs nothing. Declared before plat_hw.h, which uses it. */
static LightLock hw_ipc_lock;

#include "plat_hw.h"

_Static_assert(sizeof(LightLock) <= PLAT_MUTEX_SIZE,
               "PLAT_MUTEX_SIZE too small for LightLock");

/* The SOC service's buffer comes out of the linear heap, and libctru's default
   reservation is far larger than this app needs - every byte of it is a byte
   the guest's RAM allocation cannot have. */
u32 __ctru_linear_heap_size = 8 * 1024 * 1024;

static plat_caps_t caps;
static bool  g_new_3ds;
static char  g_model[64];

/* ------------------------------------------------------------------ life -- */

bool plat_init(void) {
  LightLock_Init(&hw_ipc_lock);

  gfxInitDefault();

  /* The bottom screen is drawn in place and never swapped - the keyboard and
     the settings page both repaint only what changed. Must happen before
     anything acquires that framebuffer. */
  gfxSetDoubleBuffering(GFX_BOTTOM, false);

  /* New3DS-only: 804MHz instead of 268MHz, plus the L2 cache. A no-op that
     simply does nothing on an Old 3DS, which is why that model needs the
     coarser cadence below. */
  osSetSpeedupEnable(true);

  APT_CheckNew3DS(&g_new_3ds);
  caps.speedup = g_new_3ds;
  snprintf(g_model, sizeof(g_model), "%s (%s UI)",
           g_new_3ds ? "New3DS" : "Old3DS", g_new_3ds ? "full" : "reduced");

  hw3ds_init();

  caps.emu_thread = true;
  caps.pointer    = true;
  caps.net        = true;
  caps.rng        = R_SUCCEEDED(psInit());
  caps.sensors    = hw.sensors;
  caps.audio      = true;
  caps.camera     = true;
  caps.mic        = true;
  caps.swkbd      = true;
  return true;
}

void plat_exit(void) {
  hw3ds_exit();
  if (caps.rng) psExit();
  gfxExit();
}

bool plat_running(void) { return aptMainLoop(); }

const plat_caps_t *plat_caps(void) { return &caps; }
const char *plat_model(void) { return g_model; }

/* An Old 3DS has no core 2, so the emulator lands on the system core and every
   main-thread wakeup competes with guest execution. Redrawing is the expensive
   half by a wide margin - a per-cell software blit of the whole grid on a
   268MHz ARM11 with no speedup available - so that model runs the same code at
   ~8fps/30Hz instead of 30fps/60Hz. Input stays well above the ~10Hz where a
   touch keyboard starts dropping taps. */
void plat_ui_cadence(uint32_t *redraw_us, uint32_t *poll_us) {
  *redraw_us = g_new_3ds ?  33000 : 120000;
  *poll_us   = g_new_3ds ? (1000000u / 60) : (1000000u / 30);
}

/* ------------------------------------------------------------------ time -- */

/* The ARM11 system tick is 268MHz regardless of osSetSpeedupEnable - the
   counter is on the fixed clock, not the CPU's. */
#define CTR_TICKS_PER_US 268

uint64_t plat_us(void) { return svcGetSystemTick() / CTR_TICKS_PER_US; }
uint64_t plat_wallclock_ms(void) { return osGetTime(); }
void plat_sleep_us(uint64_t us) { svcSleepThread((s64)us * 1000); }

/* ----------------------------------------------------------------- video -- */

/* Both panels are 24bpp BGR and rotated a quarter turn: screen pixel (x,y) is
   at byte (x * H + (H-1-y)) * 3, so a column is contiguous and y runs
   backwards through memory. base therefore points at (0,0)'s address rather
   than the allocation's. */
static void ctr_fb(plat_fb_t *out, u8 *fb, int w, int h) {
  out->base     = fb + (size_t)(h - 1) * 3;
  out->w        = w;
  out->h        = h;
  out->x_stride = h * 3;
  out->y_stride = -3;
  out->bpp      = 3;
}

bool plat_surface(plat_surf s, plat_fb_t *out) {
  if (s == PLAT_SURF_TERM) {
    ctr_fb(out, gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL),
           PLAT_TERM_W, PLAT_TERM_H);
    return true;
  }
  if (s == PLAT_SURF_PANEL) {
    ctr_fb(out, gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL),
           PLAT_PANEL_W, PLAT_PANEL_H);
    return true;
  }
  return false;
}

void plat_present(unsigned mask) {
  /* Only the top screen is double-buffered, so the panel needs no flip: it was
     drawn straight into the framebuffer the LCD is already scanning. */
  if (!(mask & PLAT_SURF_BIT(PLAT_SURF_TERM))) return;
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();
}

/* ----------------------------------------------------------------- input -- */

/* Raw circle-pad units. Below this the stick is treated as centred; the value
   is the hardware's own slop, not a UI preference. */
#define CTR_PAD_DEADZONE 40

static uint32_t ctr_map(u32 k) {
  uint32_t m = 0;
  if (k & KEY_START)  m |= PLAT_BTN_QUIT;
  if (k & KEY_SELECT) m |= PLAT_BTN_SETTINGS;
  if (k & (KEY_L | KEY_Y)) m |= PLAT_BTN_ZOOM_OUT;
  if (k & (KEY_R | KEY_X)) m |= PLAT_BTN_ZOOM_IN;
  if (k & KEY_ZL) m |= PLAT_BTN_FOLLOW;
  if (k & KEY_ZR) m |= PLAT_BTN_FONT;
  if (k & KEY_DUP)    m |= PLAT_BTN_UP;
  if (k & KEY_DDOWN)  m |= PLAT_BTN_DOWN;
  if (k & KEY_DLEFT)  m |= PLAT_BTN_LEFT;
  if (k & KEY_DRIGHT) m |= PLAT_BTN_RIGHT;
  if (k & KEY_A) m |= PLAT_BTN_A | PLAT_BTN_SWKBD;
  if (k & KEY_B) m |= PLAT_BTN_B;
  if (k & KEY_X) m |= PLAT_BTN_X;
  if (k & KEY_Y) m |= PLAT_BTN_Y;
  return m;
}

void plat_poll_input(plat_input_t *out) {
  hidScanInput();

  u32 down = hidKeysDown(), held = hidKeysHeld();
  out->down = ctr_map(down);
  out->held = ctr_map(held);

  circlePosition cp;
  hidCircleRead(&cp);
  /* Deadzone applied here rather than in core: the threshold is a property of
     this stick, and core only asks whether it is deflected. */
  out->pan_x = (abs(cp.dx) > CTR_PAD_DEADZONE) ? cp.dx : 0;
  out->pan_y = (abs(cp.dy) > CTR_PAD_DEADZONE) ? cp.dy : 0;

  touchPosition t;
  hidTouchRead(&t);
  out->ptr_valid  = true;
  out->ptr_down   = (held & KEY_TOUCH) != 0;
  out->ptr_tapped = (down & KEY_TOUCH) != 0;
  out->ptr_x = (int16_t)t.px;
  out->ptr_y = (int16_t)t.py;
}

/* No USB host on this console - the panel keyboard is the only one there
   is. See plat_poll_keyboard in source/core/plat.h. */
void plat_poll_keyboard(void) {}

/* --------------------------------------------------------------- threads -- */

static Thread      emu_thread;
static const char *emu_where = "none";

/* Prefer the New3DS's spare application core over the system core, which is
   shared with Home Menu and other background OS tasks. Both are opportunistic:
   core 2 needs exheader kernel-flag permissions belonging to whatever hosts
   this .3dsx, and core 1 needs APT_SetAppCpuTimeLimit to succeed.

   Core 2 is only asked for on a console that has one. An Old 3DS kernel does
   fail the svcCreateThread, but handing an out-of-range processor id to an
   emulator can abort on an assertion rather than returning an error. */
bool plat_thread_start(void (*entry)(void *), void *arg) {
  s32 prio = 0x30;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

  if (g_new_3ds) {
    emu_thread = threadCreate(entry, arg, 32 * 1024, prio - 1, 2, false);
    if (emu_thread) { emu_where = "New3DS core 2"; return true; }
  }
  if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))) {
    emu_thread = threadCreate(entry, arg, 32 * 1024, prio - 1, 1, false);
    if (emu_thread) { emu_where = "system core"; return true; }
  }
  return false;
}

void plat_thread_join(void) {
  if (!emu_thread) return;
  threadJoin(emu_thread, U64_MAX);
  threadFree(emu_thread);
  emu_thread = NULL;
}

const char *plat_thread_desc(void) { return emu_where; }

void plat_mutex_init(plat_mutex_t *m)   { LightLock_Init((LightLock *)m); }
void plat_mutex_lock(plat_mutex_t *m)   { LightLock_Lock((LightLock *)m); }
void plat_mutex_unlock(plat_mutex_t *m) { LightLock_Unlock((LightLock *)m); }

/* ------------------------------------------------------------------ net -- */

/* soc needs a page-aligned buffer out of linear memory that stays mapped for
   the session; it is handed to the SOC service, not used by us. */
static u32 *soc_buf;

bool plat_net_init(void) {
  if (soc_buf) return true;
  soc_buf = (u32 *)memalign(0x1000, 0x100000);
  if (!soc_buf) return false;
  if (R_FAILED(socInit(soc_buf, 0x100000))) {
    free(soc_buf);
    soc_buf = NULL;
    return false;
  }
  return true;
}

void plat_net_exit(void) {
  if (!soc_buf) return;
  socExit();
  soc_buf = NULL;   /* linearFree'd by socExit */
}

/* --------------------------------------------------------------- entropy -- */

bool plat_random(void *buf, size_t len) {
  return caps.rng && R_SUCCEEDED(PS_GenerateRandomBytes(buf, len));
}

/* --------------------------------------------------------------- sensors -- */

/* Accel and gyro come straight out of HID shared memory, so reading them every
   frame is free. The volume slider needs an IPC round trip and is a physical
   slider nobody moves 60 times a second, so it is sampled far more rarely. */
void plat_sample_axes(int32_t *out) {
  accelVector av = {0, 0, 0};
  angularRate gr = {0, 0, 0};
  if (hw.sensors) { hidAccelRead(&av); hidGyroRead(&gr); }

  static int slider_div = 0;
  static u8  vol = 0;
  if (slider_div-- <= 0) {
    slider_div = 30;
    LightLock_Lock(&hw_ipc_lock);
    Result r = HIDUSER_GetSoundVolume(&vol);
    LightLock_Unlock(&hw_ipc_lock);
    if (R_FAILED(r)) vol = 0;
  }

  out[0] = av.x; out[1] = av.y; out[2] = av.z;
  out[3] = gr.x; out[4] = gr.y; out[5] = gr.z;
  out[6] = (int32_t)(osGet3DSliderState() * 100.0f + 0.5f);
  out[7] = (int32_t)vol;
}

/* ------------------------------------------------------------- 9P trees -- */

/* The NAND trees are read-only and enforced server-side: a guest that ignored
   a mount flag would otherwise be writing system files with no filesystem
   driver on the Horizon side keeping its view consistent. */
static const plat_tree_t trees[] = {
  { "sd",   "sdmc:/",   false },
  { "nand", "v9nand:/", true  },
  { "twl",  "v9twl:/",  true  },
};

int plat_v9p_trees(const plat_tree_t **out) {
  *out = trees;
  return (int)(sizeof(trees) / sizeof(trees[0]));
}

static FS_Archive sdmc_archive;
static bool       sdmc_open;

bool plat_v9p_mount(int idx) {
  FS_Path empty = fsMakePath(PATH_EMPTY, "");
  switch (idx) {
    case 0:
      /* Held open for the session rather than reopened per call: the free-space
         query in virtio_9p.h needs the archive handle. */
      sdmc_open = R_SUCCEEDED(FSUSER_OpenArchive(&sdmc_archive, ARCHIVE_SDMC, empty));
      return sdmc_open;
    /* These need Luma3DS-style extended homebrew permissions. Without them the
       mount simply fails and the tree is absent. */
    case 1: return R_SUCCEEDED(archiveMount(ARCHIVE_NAND_CTR_FS, empty, "v9nand"));
    case 2: return R_SUCCEEDED(archiveMount(ARCHIVE_NAND_TWL_FS, empty, "v9twl"));
  }
  return false;
}

void plat_v9p_unmount_all(void) {
  archiveUnmount("v9nand");
  archiveUnmount("v9twl");
  if (sdmc_open) { FSUSER_CloseArchive(sdmc_archive); sdmc_open = false; }
}

/* Free bytes on the SD card, for the guest's statfs. -1 where unknown. */
int64_t plat_v9p_free_bytes(void) {
  u64 freeb = 0;
  if (sdmc_open && R_SUCCEEDED(FSUSER_GetFreeBytes(&freeb, sdmc_archive)))
    return (int64_t)freeb;
  return -1;
}

/* -------------------------------------------------------------- hw tree -- */

const plat_hw_ent plat_hw_files[] = {
  { "info",                0444, hw_rd_info,            NULL,       HWS_NONE },
  { "battery",             0444, hw_rd_battery,         NULL,       HWS_NONE },
  { "battery_voltage",     0444, hw_rd_battery_voltage, NULL,       HWS_NONE },
  { "charging",            0444, hw_rd_charging,        NULL,       HWS_NONE },
  { "adapter",             0444, hw_rd_adapter,         NULL,       HWS_NONE },
  { "shell",               0444, hw_rd_shell,           NULL,       HWS_NONE },
  { "steps",               0444, hw_rd_steps,           NULL,       HWS_NONE },
  { "wifi",                0444, hw_rd_wifi,            NULL,       HWS_NONE },
  { "accel",               0444, hw_rd_accel,           NULL,       HWS_NONE },
  { "gyro",                0444, hw_rd_gyro,            NULL,       HWS_NONE },
  { "slider_3d",           0444, hw_rd_slider_3d,       NULL,       HWS_NONE },
  { "slider_volume",       0444, hw_rd_slider_volume,   NULL,       HWS_NONE },
  { "model",               0444, hw_rd_model,           NULL,       HWS_NONE },
  { "region",              0444, hw_rd_region,          NULL,       HWS_NONE },
  { "language",            0444, hw_rd_language,        NULL,       HWS_NONE },
  { "firmware",            0444, hw_rd_firmware,        NULL,       HWS_NONE },
  { "console_size",        0444, hw_rd_console_size,    NULL,       HWS_NONE },
  { "leds",                0222, NULL,                  hw_wr_leds, HWS_NONE },
  { "camera_outer.rgb565", 0444, NULL,                  NULL,       HWS_CAM_OUT },
  { "camera_inner.rgb565", 0444, NULL,                  NULL,       HWS_CAM_IN  },
  { "mic.pcm",             0444, NULL,                  NULL,       HWS_MIC   },
  { "audio.pcm",           0222, NULL,                  NULL,       HWS_AUDIO },
};
const int plat_hw_count = (int)(sizeof(plat_hw_files) / sizeof(plat_hw_files[0]));

int plat_hw_camera(bool inner, uint8_t **frame) {
  uint8_t *buf = (uint8_t *)malloc(HW_CAM_BYTES);
  if (!buf) return 0;
  int got = hw_capture_frame(inner ? SELECT_IN1 : SELECT_OUT1, PORT_CAM1, buf);
  if (got <= 0) { free(buf); return 0; }
  *frame = buf;
  return got;
}

int plat_hw_mic_read(uint8_t *out, int max)          { return hw_mic_read(out, max); }
int plat_hw_audio_write(const uint8_t *d, int len)   { return hw_audio_write(d, len); }
