/* Sony PlayStation 3 backend (PSL1GHT). Implements source/core/plat.h.
 *
 * The PPU is big-endian, so the byte-reversing accessors in plat.h are live
 * here exactly as they are on the Wii, GameCube and Wii U - see PLAT_BIG_ENDIAN
 * there. It is also the first 64-bit big-endian host, but nothing in core
 * depends on host pointer width, and the accessors go byte by byte anyway.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <ppu-types.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <rsx/rsx.h>
#include <io/pad.h>
#include <io/kb.h>
#include <net/net.h>

#include "plat.h"
#include "hid_ascii.h"

/* Primary PPU thread priority and stack. The emulator thread is created in
   the same priority band - see plat_thread_start. */
SYS_PROCESS_PARAM(1001, 0x100000);

_Static_assert(sizeof(sys_lwmutex_t) <= PLAT_MUTEX_SIZE,
               "PLAT_MUTEX_SIZE too small for sys_lwmutex_t");

static plat_caps_t caps;
static char        model_str[64];
static volatile bool running = true;

/* ---------------------------------------------------------------- video -- */

/* The RSX command buffer is carved out of this host-memory region. Nothing
   else needs main memory mapped for the RSX - the only command ever issued is
   a flip - so it is sized just clear of the command buffer rather than at the
   32MB the PSL1GHT samples use, which would come straight off guest RAM. */
#define CB_SIZE   0x100000
#define HOST_SIZE (4 * 1024 * 1024)

#define SCRATCH_STRIDE (PS3_SCREEN_W * 4)
#define SCRATCH_BYTES  ((size_t)SCRATCH_STRIDE * PS3_SCREEN_H)

static gcmContextData *context;
static void           *host_addr;
static uint32_t       *color_buffer[2];
static u32             color_offset[2];
static u32             curr_fb;
static bool            flipped_once;

/* Drawing goes to main memory and is copied to the display buffer on present,
   rather than straight into the buffer the RSX is scanning. Two reasons: the
   terminal and the panel are dirty-tracked, so only the cells that changed are
   redrawn and an alternating pair of buffers would each be missing half the
   updates; and PPU reads from RSX local memory are pathologically slow, so a
   surface core might read back is much better off in XDR. */
static uint8_t *scratch;

bool plat_surface(plat_surf s, plat_fb_t *out) {
  if (!scratch) return false;
  /* base points at the R of pixel (0,0), one byte into the X8R8G8B8 word, so
     the three bytes PLAT_PX writes land on R, G and B and the X is left
     alone. Same trick as the 3DS's rotated framebuffer: the strides describe
     the layout and base is a pixel address, not the allocation's. */
  out->x_stride = 4;
  out->y_stride = SCRATCH_STRIDE;
  out->bpp      = 3;
  if (s == PLAT_SURF_TERM) {
    out->base = scratch + 1;
    out->w = PLAT_TERM_W; out->h = PLAT_TERM_H;
    return true;
  }
  if (s == PLAT_SURF_PANEL) {
    out->base = scratch + 1 + (size_t)PLAT_TERM_H * SCRATCH_STRIDE;
    out->w = PLAT_PANEL_W; out->h = PLAT_PANEL_H;
    return true;
  }
  return false;
}

static void ps3_waitflip(void) {
  while (gcmGetFlipStatus() != 0) usleep(200);
  gcmResetFlipStatus();
}

void plat_present(unsigned mask) {
  if (!mask || !scratch || !context) return;

  /* Wait before writing, not after: the buffer about to be filled is only
     free once the queued flip to the other one has actually happened. */
  if (flipped_once) ps3_waitflip(); else gcmResetFlipStatus();

  memcpy(color_buffer[curr_fb], scratch, SCRATCH_BYTES);

  gcmSetFlip(context, curr_fb);
  rsxFlushBuffer(context);
  gcmSetWaitFlip(context);

  curr_fb ^= 1;
  flipped_once = true;
}

/* 720p rather than whatever the console is set to, because PLAT_TERM_W and
   the terminal grid are compile-time: an SDTV user would otherwise get a
   1280-wide surface scanned out at 640. Every HDMI-connected PS3 can do it,
   and a component or composite one cannot do anything this app could use. */
static bool ps3_video_init(void) {
  if (videoGetResolutionAvailability(0, VIDEO_RESOLUTION_720,
                                     VIDEO_ASPECT_AUTO, 0) == 0)
    return false;

  videoConfiguration vcfg;
  memset(&vcfg, 0, sizeof(vcfg));
  vcfg.resolution = VIDEO_RESOLUTION_720;
  vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
  vcfg.aspect     = VIDEO_ASPECT_AUTO;
  vcfg.pitch      = SCRATCH_STRIDE;
  if (videoConfigure(0, &vcfg, NULL, 1) != 0) return false;

  gcmSetFlipMode(GCM_FLIP_VSYNC);

  for (int i = 0; i < 2; i++) {
    color_buffer[i] = (uint32_t *)rsxMemalign(64, SCRATCH_BYTES);
    if (!color_buffer[i]) return false;
    memset(color_buffer[i], 0, SCRATCH_BYTES);
    rsxAddressToOffset(color_buffer[i], &color_offset[i]);
    gcmSetDisplayBuffer(i, color_offset[i], SCRATCH_STRIDE,
                        PS3_SCREEN_W, PS3_SCREEN_H);
  }
  return true;
}

/* ----------------------------------------------------------------- life -- */

static void ps3_sysutil_cb(u64 status, u64 param, void *userdata) {
  (void)param; (void)userdata;
  switch (status) {
    case SYSUTIL_EXIT_GAME:
      running = false;
      break;
    /* The XMB overlay suspends this process while it is up. Without the
       rebase the guest is handed the whole suspended interval as one step
       and stalls; see g_emu_rebase_clock in plat.h. */
    case SYSUTIL_MENU_CLOSE:
    case SYSUTIL_DRAW_END:
      g_emu_rebase_clock = true;
      break;
    default:
      break;
  }
}

/* Timebase rather than the system clock: plat_us must not jump, and
   sysGetCurrentTime is the wall clock, which the XMB can move under us. */
static uint64_t tb_freq, tb_base;

static inline uint64_t ps3_tb(void) {
  uint64_t t;
  __asm__ volatile ("mftb %0" : "=r"(t));
  return t;
}

bool plat_init(void) {
  host_addr = memalign(1024 * 1024, HOST_SIZE);
  if (!host_addr) return false;
  rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr);

  if (!ps3_video_init()) {
    /* Nothing can be drawn to say so - main() only returns -1 from here - so
       this goes to the TTY, which is where ps3load and a network debug build
       will see it. */
    printf("3ds-cli: this display cannot do 720p, which is the only mode "
           "the terminal is laid out for.\n");
    return false;
  }

  scratch = (uint8_t *)memalign(128, SCRATCH_BYTES);
  if (!scratch) return false;
  memset(scratch, 0, SCRATCH_BYTES);

  tb_freq = sysGetTimebaseFrequency();
  if (!tb_freq) tb_freq = 79800000ull;   /* the PPU's, if the call ever fails */
  tb_base = ps3_tb();

  ioPadInit(7);
  /* SIXAXIS reporting is off until asked for; it is what fills
     plat_sample_axes, so a failure here is what makes caps.sensors false. */
  caps.sensors = ioPadSetSensorMode(0, 1) == 0;

  /* Brings the keyboard library up; whether anything is actually plugged in
     is asked every frame in plat_poll_keyboard, because it can change. */
  ioKbInit(MAX_KB_PORT_NUM);

  sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, ps3_sysutil_cb, NULL);

  /* Unlike every other console here the storage is not removable, so there is
     no "is a card in" question - only whether the directory exists yet. */
  mkdir(PLAT_SD, 0777);
  int entries = -1;
  DIR *d = opendir(PLAT_SD);
  if (d) { entries = 0; while (readdir(d)) entries++; closedir(d); }

  snprintf(model_str, sizeof(model_str), "%s (720p, " PLAT_SD "=%d)",
           PLAT_NAME, entries);

  caps.emu_thread = true;     /* the PPU is two-way SMT */
  caps.pointer    = false;    /* nothing points at the panel: D-pad focus */
  caps.net        = true;
  caps.rng        = false;
  caps.audio = caps.camera = caps.mic = caps.swkbd = caps.speedup = false;
  return true;
}

void plat_exit(void) {
  ioKbEnd();
  ioPadEnd();
  if (context) {
    gcmSetWaitFlip(context);
    rsxFinish(context, 1);
  }
  if (scratch) { free(scratch); scratch = NULL; }
}

bool plat_running(void) {
  /* The only place the XMB's exit request is ever noticed; core polls this
     from the main loop, which is the thread the callback has to run on. */
  sysUtilCheckCallback();
  return running;
}

const plat_caps_t *plat_caps(void) { return &caps; }
const char *plat_model(void) { return model_str; }

/* The emulator has a hardware thread of its own, so the UI can be paid for at
   full rate the way it is on the Switch. */
void plat_ui_cadence(uint32_t *redraw_us, uint32_t *poll_us) {
  *redraw_us = 16666;          /* 60fps */
  *poll_us   = 1000000u / 60;
}

/* ----------------------------------------------------------------- time -- */

uint64_t plat_us(void) {
  uint64_t d = ps3_tb() - tb_base;
  /* Split rather than d*1000000/freq: the timebase runs at ~79.8MHz, so the
     naive product overflows 64 bits about four minutes into a session. */
  return (d / tb_freq) * 1000000ull + ((d % tb_freq) * 1000000ull) / tb_freq;
}

uint64_t plat_wallclock_ms(void) {
  u64 sec = 0, nsec = 0;
  sysGetCurrentTime(&sec, &nsec);
  return sec * 1000ull + nsec / 1000000ull;
}

void plat_sleep_us(uint64_t us) {
  while (us > 0xF0000000ull) { sysUsleep(0xF0000000u); us -= 0xF0000000ull; }
  sysUsleep((u32)us);
}

/* ---------------------------------------------------------------- input -- */

/* The sticks report 0..255 about a centre of 128. */
#define PS3_STICK_CENTRE   128
#define PS3_STICK_DEADZONE 40

static uint32_t ps3_map(const padData *p) {
  uint32_t m = 0;
  if (p->BTN_START)  m |= PLAT_BTN_QUIT;
  if (p->BTN_SELECT) m |= PLAT_BTN_SETTINGS;
  /* This is the first pad here with all four shoulders, so ZL and ZR land
     where the 3DS has them instead of riding on face buttons. */
  if (p->BTN_L1) m |= PLAT_BTN_ZOOM_OUT;
  if (p->BTN_R1) m |= PLAT_BTN_ZOOM_IN;
  if (p->BTN_L2) m |= PLAT_BTN_FOLLOW;
  if (p->BTN_R2) m |= PLAT_BTN_FONT;
  if (p->BTN_UP)    m |= PLAT_BTN_UP;
  if (p->BTN_DOWN)  m |= PLAT_BTN_DOWN;
  if (p->BTN_LEFT)  m |= PLAT_BTN_LEFT;
  if (p->BTN_RIGHT) m |= PLAT_BTN_RIGHT;
  /* Cross confirms and circle goes back, as everywhere else on this pad.
     Triangle and square carry X and Y so the README's "R/X in, L/Y out"
     still holds. */
  if (p->BTN_CROSS)    m |= PLAT_BTN_A;
  if (p->BTN_CIRCLE)   m |= PLAT_BTN_B;
  if (p->BTN_TRIANGLE) m |= PLAT_BTN_X | PLAT_BTN_ZOOM_IN;
  if (p->BTN_SQUARE)   m |= PLAT_BTN_Y | PLAT_BTN_ZOOM_OUT;
  return m;
}

/* ioPadGetData reports level, not edges, so the backend keeps the previous
   frame to derive `down` - libogc and libnx do this for their callers. */
static uint32_t prev_held;
static padData  last_pad;
static bool     pad_ok;

void plat_poll_input(plat_input_t *out) {
  memset(out, 0, sizeof(*out));

  padInfo info;
  if (ioPadGetInfo(&info) != 0 || !info.status[0]) { prev_held = 0; pad_ok = false; return; }

  padData p;
  if (ioPadGetData(0, &p) != 0 || !p.len) { prev_held = 0; pad_ok = false; return; }
  last_pad = p;
  pad_ok   = true;

  uint32_t held = ps3_map(&p);
  out->held = held;
  out->down = held & ~prev_held;
  prev_held = held;

  int sx = (int)p.ANA_L_H - PS3_STICK_CENTRE;
  int sy = (int)p.ANA_L_V - PS3_STICK_CENTRE;
  out->pan_x = (sx >  PS3_STICK_DEADZONE) ?  1 : (sx < -PS3_STICK_DEADZONE) ? -1 : 0;
  /* The stick reads larger downwards; the viewport pans the other way. */
  out->pan_y = (sy >  PS3_STICK_DEADZONE) ?  1 : (sy < -PS3_STICK_DEADZONE) ? -1 : 0;
}

/* ------------------------------------------------------------- keyboard -- */

/* lv2 will do the layout translation itself, in whatever mapping the user set
   in the XMB, which is better than anything this could infer from a usage id.
   So the port is asked for ASCII and hid_ascii.h is only consulted for the
   keys that have no character - arrows, function keys - which come back with
   KB_RAWDAT set and the raw usage in the low byte. */
static bool kb_configured;

static void ps3_kb_configure(void) {
  ioKbSetCodeType(0, KB_CODETYPE_ASCII);
  /* Character mode rather than packet mode: lv2 does the key repeat and hands
     over typed characters, so there is no edge detection to do here. */
  ioKbSetReadMode(0, KB_RMODE_INPUTCHAR);
  kb_configured = true;
}

void plat_poll_keyboard(void) {
  KbInfo info;
  if (ioKbGetInfo(&info) != 0 || info.connected == 0) {
    /* Unplugged. Forget the configuration too: the next keyboard to arrive is
       a fresh port that has never been told what code type we want. */
    caps.keyboard = false;
    kb_configured = false;
    return;
  }
  caps.keyboard = true;
  if (!kb_configured) ps3_kb_configure();

  KbData d;
  if (ioKbRead(0, &d) != 0 || d.nb_keycode <= 0) return;

  bool ctrl  = d.mkey._KbMkeyU._KbMkeyS.l_ctrl  || d.mkey._KbMkeyU._KbMkeyS.r_ctrl;
  bool shift = d.mkey._KbMkeyU._KbMkeyS.l_shift || d.mkey._KbMkeyU._KbMkeyS.r_shift;

  for (int i = 0; i < d.nb_keycode; i++) {
    uint16_t k = d.keycode[i];
    if (k == 0) continue;

    if (k & KB_RAWDAT) {
      char buf[8];
      const char *s = hid_term_bytes((uint8_t)(k & 0xFF), shift, ctrl, false, buf);
      if (s) rx_push_str(s);
      continue;
    }

    char c = (char)(k & 0xFF);
    if (!c) continue;
    /* Ctrl is not folded in for us even in ASCII mode, and a terminal without
       ctrl-c is not much of a terminal. */
    if (ctrl) {
      if (c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 1);
      else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
      else if (c == ' ')             c = 0;
      else if (c == '[')             c = 0x1b;
      else if (c == '\\')            c = 0x1c;
      else if (c == ']')             c = 0x1d;
    }
    rx_push(c);
  }
}

/* -------------------------------------------------------------- threads -- */

static sys_ppu_thread_t emu_tid;
static bool             emu_started;

bool plat_thread_start(void (*entry)(void *), void *arg) {
  /* Same priority band as the main thread: both PPU hardware threads are
     available, so there is nothing to gain by making one wait on the other. */
  if (sysThreadCreate(&emu_tid, entry, arg, 1001, 0x20000,
                      THREAD_JOINABLE, "3dscli-emu") != 0)
    return false;
  emu_started = true;
  return true;
}

void plat_thread_join(void) {
  if (!emu_started) return;
  u64 retval = 0;
  sysThreadJoin(emu_tid, &retval);
  emu_started = false;
}

const char *plat_thread_desc(void) { return "PPU thread 1"; }

void plat_mutex_init(plat_mutex_t *m) {
  sys_lwmutex_attr_t attr;
  attr.attr_protocol  = SYS_LWMUTEX_PROTOCOL_PRIO;
  attr.attr_recursive = SYS_LWMUTEX_ATTR_NOT_RECURSIVE;
  /* name is char[8] and lv2 wants it NUL-terminated. */
  strncpy(attr.name, "3dscli", sizeof(attr.name));
  sysLwMutexCreate((sys_lwmutex_t *)m, &attr);
}

void plat_mutex_lock(plat_mutex_t *m)   { sysLwMutexLock((sys_lwmutex_t *)m, 0); }
void plat_mutex_unlock(plat_mutex_t *m) { sysLwMutexUnlock((sys_lwmutex_t *)m); }

/* ------------------------------------------------------------------ net -- */

/* The console is already configured by the XMB, so there is no association
   step here the way there is on the 3DS or the PSP - the stack just comes up
   on whatever connection the user set. */
bool plat_net_init(void) { return netInitialize() >= 0; }
void plat_net_exit(void)  { netDeinitialize(); }

/* -------------------------------------------------------------- entropy -- */

bool plat_random(void *buf, size_t len) { (void)buf; (void)len; return false; }

/* -------------------------------------------------------------- sensors -- */

/* SIXAXIS accelerometer, 0..1023 about a centre of 512. There is a gyro in
   the pad too, but only a single yaw axis (SENSOR_G), and it reads as noise
   when the pad is still, so it is left at 0 rather than reported. */
void plat_sample_axes(int32_t *out) {
  memset(out, 0, sizeof(int32_t) * VI_NAXES);
  if (!caps.sensors || !pad_ok) return;
  out[0] = (int32_t)last_pad.SENSOR_X - 512;
  out[1] = (int32_t)last_pad.SENSOR_Y - 512;
  out[2] = (int32_t)last_pad.SENSOR_Z - 512;
}

/* ------------------------------------------------------------- 9P trees -- */

static const plat_tree_t trees[] = {
  { "sd", PLAT_SD, false },
};

int plat_v9p_trees(const plat_tree_t **out) {
  *out = trees;
  return (int)(sizeof(trees) / sizeof(trees[0]));
}

/* Nothing to mount: lv2 has the drive up before the app starts, and plat_init
   has already made the directory. */
bool plat_v9p_mount(int idx) { return idx == 0; }
void plat_v9p_unmount_all(void) {}
int64_t plat_v9p_free_bytes(void) { return -1; }

/* -------------------------------------------------------------- hw tree -- */

static int hw_rd_console_size(char *b, int n) {
  return snprintf(b, n, "%d %d\n", TERM_COLS, TERM_ROWS);
}
static int hw_rd_model(char *b, int n) { return snprintf(b, n, "%s\n", PLAT_NAME); }

static int hw_rd_accel(char *b, int n) {
  if (!caps.sensors || !pad_ok) return snprintf(b, n, "unknown\n");
  return snprintf(b, n, "%d %d %d\n",
                  (int)last_pad.SENSOR_X - 512,
                  (int)last_pad.SENSOR_Y - 512,
                  (int)last_pad.SENSOR_Z - 512);
}

static int hw_rd_info(char *b, int n) {
  return snprintf(b, n, "model:    %s\nscreen:   %dx%d\nconsole:  %dx%d\n",
                  PLAT_NAME, PS3_SCREEN_W, PS3_SCREEN_H, TERM_COLS, TERM_ROWS);
}

const plat_hw_ent plat_hw_files[] = {
  { "info",         0444, hw_rd_info,         NULL, HWS_NONE },
  { "model",        0444, hw_rd_model,        NULL, HWS_NONE },
  { "accel",        0444, hw_rd_accel,        NULL, HWS_NONE },
  { "console_size", 0444, hw_rd_console_size, NULL, HWS_NONE },
};
const int plat_hw_count = (int)(sizeof(plat_hw_files) / sizeof(plat_hw_files[0]));

int plat_hw_camera(bool inner, uint8_t **frame) { (void)inner; (void)frame; return 0; }
int plat_hw_mic_read(uint8_t *out, int max)     { (void)out; (void)max; return 0; }
int plat_hw_audio_write(const uint8_t *d, int l) { (void)d; return l; }
